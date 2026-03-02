#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <bthread/butex.h>
#include <bthread/bthread.h>
#include <bthread/unstable.h>
#include <butil/logging.h>
#include <gflags/gflags.h>
#include <google/protobuf/stubs/common.h>

#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

#include <brpc/server.h>

#include "file_read.pb.h"

DECLARE_int32(task_group_ntags);

DEFINE_int32(port, 8002, "Server listen port");
DEFINE_int32(monitor_port, 8003, "Builtin monitor service port");
DEFINE_int32(read_tag, 1, "bthread tag for file read requests");
DEFINE_int32(monitor_tag, 0, "bthread tag for monitor builtin services");
DEFINE_int32(read_num_threads, 12,
             "pthread workers hint for read server (default: 12)");
DEFINE_int32(monitor_num_threads, 4,
             "pthread workers hint for monitor server (default: 4)");
DEFINE_string(file_path, "/tmp/iouring-read-default.txt",
              "Default file path when request.path is empty");
DEFINE_int32(max_read_len, 1 << 20, "Maximum readable bytes per request");
DEFINE_int32(read_timeout_ms, 2000, "RPC wait_local timeout in milliseconds");
DEFINE_int32(direct_io_align, 4096, "Alignment bytes for O_DIRECT reads");
DEFINE_int32(max_aligned_read_len, 4 << 20,
             "Maximum aligned bytes submitted to io_uring per request");
DEFINE_int32(iouring_batch_cqe_max, 128,
             "Max CQEs to peek in one harvest round");
DEFINE_bool(iouring_setup_coop_taskrun, true,
            "Enable IORING_SETUP_COOP_TASKRUN when supported");
DEFINE_bool(iouring_setup_single_issuer, true,
            "Enable IORING_SETUP_SINGLE_ISSUER when supported");
DEFINE_bool(iouring_setup_defer_taskrun, true,
            "Enable IORING_SETUP_DEFER_TASKRUN when supported");
DEFINE_bool(iouring_setup_sqpoll, false,
            "Enable IORING_SETUP_SQPOLL when supported");
DEFINE_int32(iouring_sq_thread_idle_ms, 2000,
             "sq_thread_idle for IORING_SETUP_SQPOLL");

namespace {

struct ReqCtx {
  ReqCtx() : done_butex(bthread::butex_create()) {
    if (done_butex != nullptr) {
      static_cast<butil::atomic<int>*>(done_butex)->store(0,
                                                          butil::memory_order_relaxed);
    }
  }
  ~ReqCtx() {
    if (aligned_buffer != nullptr) {
      free(aligned_buffer);
      aligned_buffer = nullptr;
    }
    if (done_butex != nullptr) {
      bthread::butex_destroy(static_cast<butil::atomic<int>*>(done_butex));
      done_butex = nullptr;
    }
  }

  int PrepareAlignedBuffer(size_t alignment, size_t length) {
    aligned_len = length;
    if (aligned_len == 0) {
      return EINVAL;
    }
    if (posix_memalign(&aligned_buffer, alignment, aligned_len) != 0) {
      aligned_buffer = nullptr;
      return ENOMEM;
    }
    memset(aligned_buffer, 0, aligned_len);
    return 0;
  }

  void* done_butex = nullptr;
  void* aligned_buffer = nullptr;
  size_t aligned_len = 0;
  size_t prefix_skip = 0;
  size_t request_len = 0;
  ssize_t io_result = 0;
  int io_errno = 0;
  int wake_errno = 0;
};

class WorkerIoState {
 public:
  WorkerIoState() : initialized_(false) { memset(&ring_, 0, sizeof(ring_)); }

  ~WorkerIoState() { Destroy(); }

  int Init() {
    if (initialized_) {
      return 0;
    }
    constexpr unsigned kQueueDepth = 512;
    io_uring_params p;
    memset(&p, 0, sizeof(p));
#ifdef IORING_SETUP_COOP_TASKRUN
    if (FLAGS_iouring_setup_coop_taskrun) {
      p.flags |= IORING_SETUP_COOP_TASKRUN;
    }
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
    if (FLAGS_iouring_setup_single_issuer) {
      p.flags |= IORING_SETUP_SINGLE_ISSUER;
    }
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
    if (FLAGS_iouring_setup_defer_taskrun) {
      p.flags |= IORING_SETUP_DEFER_TASKRUN;
    }
#endif
#ifdef IORING_SETUP_SQPOLL
    if (FLAGS_iouring_setup_sqpoll) {
      p.flags |= IORING_SETUP_SQPOLL;
      p.sq_thread_idle = FLAGS_iouring_sq_thread_idle_ms;
    }
#endif
    int rc = io_uring_queue_init_params(kQueueDepth, &ring_, &p);
    if (rc < 0) {
      // Fall back to baseline setup when tuned flags are not available in the runtime.
      rc = io_uring_queue_init(kQueueDepth, &ring_, 0);
      if (rc < 0) {
        return -rc;
      }
    }
    initialized_ = true;
    return 0;
  }

  void Destroy() {
    if (!initialized_) {
      return;
    }
    io_uring_queue_exit(&ring_);
    initialized_ = false;
  }

  int QueueRead(int fd, ReqCtx* req, off_t offset) {
    if (req == nullptr || req->aligned_buffer == nullptr || req->aligned_len == 0) {
      return EINVAL;
    }
    io_uring_sqe* sqe = nullptr;
    // If SQ is temporarily full, force one submit and retry once.
    for (int attempt = 0; attempt < 2; ++attempt) {
      sqe = io_uring_get_sqe(&ring_);
      if (sqe != nullptr) {
        break;
      }
      const int submit_rc = io_uring_submit(&ring_);
      if (submit_rc < 0) {
        return -submit_rc;
      }
    }
    if (sqe == nullptr) {
      return EBUSY;
    }
    io_uring_prep_read(sqe, fd, req->aligned_buffer, req->aligned_len, offset);
    io_uring_sqe_set_data(sqe, req);
    return 0;
  }

  int Harvest(const bthread_active_task_ctx_t* wake_ctx) {
    bool made_progress = false;
    if (io_uring_sq_ready(&ring_) > 0) {
      const int submit_rc = io_uring_submit(&ring_);
      if (submit_rc < 0) {
        LOG(ERROR) << "io_uring_submit in harvest failed, rc=" << submit_rc;
      } else {
        made_progress = true;
      }
    }

    const unsigned batch_max = static_cast<unsigned>(std::max(1, FLAGS_iouring_batch_cqe_max));
    std::vector<io_uring_cqe*> cqes(batch_max, nullptr);
    while (true) {
      const unsigned n = io_uring_peek_batch_cqe(&ring_, cqes.data(), batch_max);
      if (n == 0) {
        break;
      }
      unsigned advanced = 0;
      bool blocked_on_wake_eagain = false;
      for (unsigned i = 0; i < n; ++i) {
        io_uring_cqe* cqe = cqes[i];
        ReqCtx* req = static_cast<ReqCtx*>(io_uring_cqe_get_data(cqe));
        if (req != nullptr && req->done_butex != nullptr) {
          if (cqe->res >= 0) {
            req->io_result = cqe->res;
            req->io_errno = 0;
          } else {
            req->io_result = -1;
            req->io_errno = -cqe->res;
          }
          static_cast<butil::atomic<int>*>(req->done_butex)
              ->store(1, butil::memory_order_release);
          const int wake_rc = bthread_butex_wake_within(wake_ctx, req->done_butex);
          if (wake_rc == 1 || wake_rc == 0) {
            if (wake_rc == 1) {
              made_progress = true;
            }
          } else if (wake_rc == -1 && errno == EAGAIN) {
            // pin_rq is temporarily full: keep this CQE for next harvest.
            blocked_on_wake_eagain = true;
            break;
          } else {
            req->wake_errno = errno;
          }
        }
        ++advanced;
      }
      if (advanced > 0) {
        io_uring_cq_advance(&ring_, advanced);
        made_progress = true;
      }
      if (blocked_on_wake_eagain) {
        break;
      }
    }
    return made_progress ? 1 : 0;
  }

 private:
  io_uring ring_;
  bool initialized_;
};

thread_local WorkerIoState* g_worker_io = nullptr;

int WorkerInit(void** worker_local, const bthread_active_task_ctx_t*, void*) {
  WorkerIoState* state = new WorkerIoState();
  const int rc = state->Init();
  if (rc != 0) {
    LOG(ERROR) << "worker io_uring init failed, rc=" << rc;
    delete state;
    return rc;
  }
  *worker_local = state;
  g_worker_io = state;
  return rc;
}

void WorkerDestroy(void* worker_local, const bthread_active_task_ctx_t*, void*) {
  WorkerIoState* state = static_cast<WorkerIoState*>(worker_local);
  if (g_worker_io == state) {
    g_worker_io = nullptr;
  }
  delete state;
}

int WorkerHarvest(void* worker_local, const bthread_active_task_ctx_t* ctx) {
  WorkerIoState* state = static_cast<WorkerIoState*>(worker_local);
  if (state == nullptr) {
    return 0;
  }
  return state->Harvest(ctx);
}

int RegisterActiveTaskRuntime() {
  bthread_active_task_type_t type;
  memset(&type, 0, sizeof(type));
  type.struct_size = sizeof(type);
  type.name = "iouring_file_read";
  type.worker_init = &WorkerInit;
  type.worker_destroy = &WorkerDestroy;
  type.harvest = &WorkerHarvest;
  return bthread_register_active_task_type(&type);
}

bool IsPowerOfTwo(size_t x) { return x != 0 && (x & (x - 1)) == 0; }

uint64_t AlignDown(uint64_t v, uint64_t align) { return v & ~(align - 1); }

uint64_t AlignUp(uint64_t v, uint64_t align) { return (v + align - 1) & ~(align - 1); }

class FileReadServiceImpl : public iouring_file_read::FileReadService {
 public:
  void Read(::google::protobuf::RpcController* cntl_base,
            const iouring_file_read::FileReadRequest* req,
            iouring_file_read::FileReadResponse* resp,
            ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);

    if (req->offset() < 0) {
      return SetInvalidArgument(resp, "offset must be non-negative");
    }
    if (req->len() <= 0) {
      return SetInvalidArgument(resp, "len must be positive");
    }
    if (req->len() > FLAGS_max_read_len) {
      return SetInvalidArgument(resp, "len exceeds max_read_len");
    }
    if (req->offset() >
        (std::numeric_limits<int64_t>::max() - static_cast<int64_t>(req->len()))) {
      return SetInvalidArgument(resp, "offset + len overflow");
    }

    const size_t alignment = static_cast<size_t>(FLAGS_direct_io_align);
    if (!IsPowerOfTwo(alignment) || alignment < 512) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("direct_io_align must be power-of-two and >= 512");
      resp->clear_body();
      return;
    }

    const std::string path = !req->path().empty() ? req->path() : FLAGS_file_path;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
    if (fd < 0) {
      return SetIoError(resp, errno, "open failed");
    }

    const uint64_t request_offset = static_cast<uint64_t>(req->offset());
    const uint64_t request_len = static_cast<uint64_t>(req->len());
    const uint64_t request_end = request_offset + request_len;
    const uint64_t aligned_offset = AlignDown(request_offset, alignment);
    const uint64_t aligned_end = AlignUp(request_end, alignment);
    const uint64_t aligned_len = aligned_end - aligned_offset;
    if (aligned_len == 0 || aligned_len > static_cast<uint64_t>(FLAGS_max_aligned_read_len)) {
      close(fd);
      return SetInvalidArgument(resp, "aligned read length out of range");
    }

    ReqCtx request_ctx;
    if (request_ctx.done_butex == nullptr) {
      close(fd);
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("failed to create private butex");
      return;
    }
    request_ctx.request_len = static_cast<size_t>(request_len);
    request_ctx.prefix_skip = static_cast<size_t>(request_offset - aligned_offset);
    const int prep_rc =
        request_ctx.PrepareAlignedBuffer(alignment, static_cast<size_t>(aligned_len));
    if (prep_rc != 0) {
      close(fd);
      return SetIoError(resp, prep_rc, "allocate aligned buffer failed");
    }
    if (!g_worker_io) {
      close(fd);
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("worker io state unavailable");
      return;
    }
    const int submit_rc =
        g_worker_io->QueueRead(fd, &request_ctx, static_cast<off_t>(aligned_offset));
    if (submit_rc != 0) {
      close(fd);
      return SetIoError(resp, submit_rc, "io_uring submit read failed");
    }

    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const int64_t timeout_ns =
        static_cast<int64_t>(FLAGS_read_timeout_ms) * 1000 * 1000;
    ts.tv_sec += timeout_ns / 1000000000LL;
    ts.tv_nsec += timeout_ns % 1000000000LL;
    if (ts.tv_nsec >= 1000000000LL) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000LL;
    }

    const int wait_rc = bthread_butex_wait_local(request_ctx.done_butex, 0, &ts);
    const int butex_value =
        static_cast<butil::atomic<int>*>(request_ctx.done_butex)
            ->load(butil::memory_order_acquire);
    if (wait_rc != 0 && errno != EWOULDBLOCK && butex_value == 0) {
      const int saved_errno = errno;
      close(fd);
      if (saved_errno == ETIMEDOUT) {
        resp->set_code(iouring_file_read::FILE_READ_TIMEOUT);
        resp->set_message("wait_local timeout");
      } else {
        SetIoError(resp, saved_errno, "wait_local failed");
      }
      return;
    }

    close(fd);

    if (request_ctx.wake_errno != 0) {
      SetIoError(resp, request_ctx.wake_errno, "wake_within failed");
      return;
    }
    if (request_ctx.io_result < 0) {
      SetIoError(resp, request_ctx.io_errno, "read failed");
      return;
    }

    resp->set_code(iouring_file_read::FILE_READ_OK);
    if (request_ctx.io_result <= static_cast<ssize_t>(request_ctx.prefix_skip)) {
      resp->clear_body();
    } else {
      const size_t readable_after_skip =
          static_cast<size_t>(request_ctx.io_result) - request_ctx.prefix_skip;
      const size_t body_len = std::min(readable_after_skip, request_ctx.request_len);
      const char* body_ptr =
          static_cast<const char*>(request_ctx.aligned_buffer) + request_ctx.prefix_skip;
      resp->set_body(body_ptr, body_len);
    }
    resp->set_message("ok");
    (void)cntl_base;
  }

 private:
  void SetInvalidArgument(iouring_file_read::FileReadResponse* resp,
                          const std::string& msg) {
    resp->set_code(iouring_file_read::FILE_READ_INVALID_ARGUMENT);
    resp->set_message(msg);
    resp->clear_body();
  }

  void SetIoError(iouring_file_read::FileReadResponse* resp, int err,
                  const std::string& prefix) {
    resp->set_code(err);
    resp->set_message(prefix + ": " + std::string(strerror(err)));
    resp->clear_body();
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  constexpr int kMinTagConcurrency = 4;
  if (FLAGS_read_num_threads < kMinTagConcurrency) {
    LOG(WARNING) << "read_num_threads(" << FLAGS_read_num_threads
                 << ") < " << kMinTagConcurrency << ", bump to "
                 << kMinTagConcurrency;
    FLAGS_read_num_threads = kMinTagConcurrency;
  }
  if (FLAGS_monitor_num_threads < kMinTagConcurrency) {
    LOG(WARNING) << "monitor_num_threads(" << FLAGS_monitor_num_threads
                 << ") < " << kMinTagConcurrency << ", bump to "
                 << kMinTagConcurrency;
    FLAGS_monitor_num_threads = kMinTagConcurrency;
  }
  const int needed_ntags = std::max(FLAGS_read_tag, FLAGS_monitor_tag) + 1;
  if (FLAGS_task_group_ntags < needed_ntags) {
    LOG(WARNING) << "task_group_ntags(" << FLAGS_task_group_ntags
                 << ") is smaller than required tags(" << needed_ntags
                 << "), auto bump to " << needed_ntags;
    FLAGS_task_group_ntags = needed_ntags;
  }

  const int runtime_rc = RegisterActiveTaskRuntime();
  if (runtime_rc != 0) {
    LOG(ERROR) << "register active-task runtime failed, rc=" << runtime_rc
               << ", unstable APIs might be unavailable on this checkout";
    return 1;
  }

  brpc::Server read_server;
  brpc::Server monitor_server;
  FileReadServiceImpl svc;
  if (read_server.AddService(&svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "Fail to add service";
    return -1;
  }

  brpc::ServerOptions monitor_options;
  monitor_options.bthread_tag = FLAGS_monitor_tag;
  monitor_options.has_builtin_services = true;
  int monitor_threads = FLAGS_monitor_num_threads;
  const int current_monitor_threads =
      bthread_getconcurrency_by_tag(static_cast<bthread_tag_t>(FLAGS_monitor_tag));
  if (current_monitor_threads > monitor_threads) {
    LOG(WARNING) << "monitor tag " << FLAGS_monitor_tag
                 << " current concurrency is " << current_monitor_threads
                 << ", cannot decrease to " << monitor_threads
                 << ", use " << current_monitor_threads;
    monitor_threads = current_monitor_threads;
  }
  monitor_options.num_threads = monitor_threads;
  if (monitor_server.Start(FLAGS_monitor_port, &monitor_options) != 0) {
    LOG(ERROR) << "Fail to start monitor server";
    return -1;
  }

  brpc::ServerOptions read_options;
  read_options.bthread_tag = FLAGS_read_tag;
  read_options.has_builtin_services = false;
  int read_threads = FLAGS_read_num_threads;
  const int current_read_threads =
      bthread_getconcurrency_by_tag(static_cast<bthread_tag_t>(FLAGS_read_tag));
  if (current_read_threads > read_threads) {
    LOG(WARNING) << "read tag " << FLAGS_read_tag
                 << " current concurrency is " << current_read_threads
                 << ", cannot decrease to " << read_threads
                 << ", use " << current_read_threads;
    read_threads = current_read_threads;
  }
  read_options.num_threads = read_threads;
  if (read_server.Start(FLAGS_port, &read_options) != 0) {
    LOG(ERROR) << "Fail to start read server";
    return -1;
  }

  read_server.RunUntilAskedToQuit();
  monitor_server.RunUntilAskedToQuit();
  return 0;
}
