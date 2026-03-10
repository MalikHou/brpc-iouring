#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <liburing.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>

#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

#include <brpc/server.h>
#include <brpc/event_dispatcher.h>
#include <bthread/bthread.h>
#include <bthread/butex.h>
#include <bthread/unstable.h>
#include <butil/logging.h>
#include <gflags/gflags.h>

#include "file_read.pb.h"

DECLARE_int32(task_group_ntags);

namespace bthread {
DECLARE_int64(bthread_active_task_idle_wait_ns);
DECLARE_int32(bthread_active_task_poll_every_nswitch);
}  // namespace bthread

DEFINE_int32(port, 8002, "io_uring server listen port");
DEFINE_int32(pread_port, 8003, "pread server listen port");
DEFINE_int32(monitor_port, 8010, "monitor server listen port");
DEFINE_string(file_path, "/tmp/read_bench.data", "file path used for reads");
DEFINE_int32(file_open_flags, O_RDONLY | O_CLOEXEC,
             "flags used to open --file_path");
DEFINE_bool(file_direct_io, false,
            "append O_DIRECT to --file_open_flags when opening --file_path");
DEFINE_int32(max_read_size, 64 * 1024, "max bytes per request");
DEFINE_int32(iouring_queue_depth, 512, "io_uring queue depth per worker");
DEFINE_int32(num_threads, 5, "brpc worker threads for io_uring/pread servers");
DEFINE_int32(monitor_num_threads, 5, "brpc worker threads for monitor server");
DEFINE_int32(iouring_harvest_batch, 64, "max CQEs harvested per round");
DEFINE_int32(monitor_tag, 0, "bthread tag for monitor server");
DEFINE_int32(iouring_tag, 1, "bthread tag for io_uring server");
DEFINE_int32(pread_tag, 2, "bthread tag for pread server");

namespace {

constexpr int kReadErrorLogEveryN = 1000;
constexpr size_t kDirectIoDefaultAlign = 4096;

thread_local class WorkerIoRing* tls_worker_ring = nullptr;
int g_file_fd = -1;
bool g_use_direct_io = false;
size_t g_direct_io_mem_align = 1;
size_t g_direct_io_offset_align = 1;
int g_effective_file_open_flags = 0;

struct DirectIoAlign {
  size_t mem_align = kDirectIoDefaultAlign;
  size_t offset_align = kDirectIoDefaultAlign;
};

bool IsValidDirectIoAlign(size_t align) {
  return align >= sizeof(void*) &&
         (align & (align - 1)) == 0;
}

bool IsKernelVersionAtLeast(int target_major, int target_minor) {
  struct utsname uts;
  std::memset(&uts, 0, sizeof(uts));
  if (uname(&uts) != 0) {
    return false;
  }
  int major = 0;
  int minor = 0;
  if (std::sscanf(uts.release, "%d.%d", &major, &minor) != 2) {
    return false;
  }
  if (major != target_major) {
    return major > target_major;
  }
  return minor >= target_minor;
}

unsigned ResolveIouringInitFlags() {
  static const unsigned kFlags = []() -> unsigned {
    if (!IsKernelVersionAtLeast(6, 6)) {
      return 0;
    }
    unsigned flags = 0;
#ifdef IORING_SETUP_SINGLE_ISSUER
    flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_COOP_TASKRUN
    flags |= IORING_SETUP_COOP_TASKRUN;
#endif
    return flags;
  }();
  return kFlags;
}

struct IoRequest {
  IoRequest() : done_butex(bthread::butex_create()) {
    if (done_butex != nullptr) {
      static_cast<butil::atomic<int>*>(done_butex)
          ->store(0, butil::memory_order_relaxed);
    }
  }

  ~IoRequest() {
    if (done_butex != nullptr) {
      bthread::butex_destroy(static_cast<butil::atomic<int>*>(done_butex));
      done_butex = nullptr;
    }
    if (aligned_buffer != nullptr) {
      std::free(aligned_buffer);
      aligned_buffer = nullptr;
      aligned_buffer_capacity = 0;
    }
  }

  IoRequest(const IoRequest&) = delete;
  IoRequest& operator=(const IoRequest&) = delete;

  int PrepareBuffer(uint32_t len, bool use_direct_io, size_t mem_align) {
    if (len == 0) {
      return EINVAL;
    }

    if (!use_direct_io) {
      buffer.resize(len);
      use_aligned_buffer = false;
      return 0;
    }

    if (!IsValidDirectIoAlign(mem_align)) {
      return EINVAL;
    }
    if (aligned_buffer == nullptr || aligned_buffer_capacity < len) {
      void* ptr = nullptr;
      const int rc = posix_memalign(&ptr, mem_align, len);
      if (rc != 0) {
        return rc;
      }
      std::free(aligned_buffer);
      aligned_buffer = ptr;
      aligned_buffer_capacity = len;
    }
    use_aligned_buffer = true;
    return 0;
  }

  char* MutableData() {
    return use_aligned_buffer ? static_cast<char*>(aligned_buffer) : buffer.data();
  }

  const char* Data() const {
    return use_aligned_buffer ? static_cast<const char*>(aligned_buffer) : buffer.data();
  }

  void* done_butex = nullptr;
  std::string buffer;
  void* aligned_buffer = nullptr;
  size_t aligned_buffer_capacity = 0;
  bool use_aligned_buffer = false;
  ssize_t result = -1;
  int io_errno = 0;
  int wake_errno = 0;
};

class WorkerIoRing {
 public:
  WorkerIoRing() = default;

  ~WorkerIoRing() { Destroy(); }

  int Init() {
    if (initialized_) {
      return 0;
    }
    if (FLAGS_iouring_queue_depth <= 0) {
      return EINVAL;
    }
    const unsigned optimized_flags = ResolveIouringInitFlags();
    if (optimized_flags != 0) {
      const int opt_rc =
          io_uring_queue_init(FLAGS_iouring_queue_depth, &ring_, optimized_flags);
      if (opt_rc == 0) {
        initialized_ = true;
        LOG(INFO) << "io_uring_queue_init with optimized flags succeeded"
                  << ", flags=0x" << std::hex << optimized_flags << std::dec;
        return 0;
      }
      const int err = -opt_rc;
      LOG(WARNING) << "io_uring_queue_init with optimized flags failed"
                   << ", flags=0x" << std::hex << optimized_flags << std::dec
                   << ", err=" << err << "(" << std::strerror(err) << ")"
                   << ", fallback to flags=0";
    }

    const int rc = io_uring_queue_init(FLAGS_iouring_queue_depth, &ring_, 0);
    if (rc < 0) {
      return -rc;
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

  int QueueRead(int fd, uint64_t offset, uint32_t len, IoRequest* req) {
    if (req == nullptr || req->done_butex == nullptr || len == 0) {
      return EINVAL;
    }
    if (!initialized_) {
      const int init_rc = Init();
      if (init_rc != 0) {
        return init_rc;
      }
    }
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      return EBUSY;
    }
    const int prep_rc = req->PrepareBuffer(len, g_use_direct_io, g_direct_io_mem_align);
    if (prep_rc != 0) {
      return prep_rc;
    }
    io_uring_prep_read(sqe, fd, req->MutableData(), len, static_cast<off_t>(offset));
    io_uring_sqe_set_data(sqe, req);
    return 0;
  }

  int Harvest(const bthread_active_task_ctx_t* wake_ctx) {
    if (!initialized_) {
      return 0;
    }
    bool made_progress = false;
    if (io_uring_sq_ready(&ring_) > 0) {
      const int submit_rc = io_uring_submit(&ring_);
      if (submit_rc < 0) {
        LOG(ERROR) << "io_uring_submit failed, rc=" << submit_rc;
      } else if (submit_rc > 0) {
        made_progress = true;
      }
    }

    const unsigned batch = static_cast<unsigned>(
        std::max(1, FLAGS_iouring_harvest_batch));
    if (batch_cqes_.size() < batch) {
      batch_cqes_.resize(batch);
    }
    const unsigned nr =
        io_uring_peek_batch_cqe(&ring_, batch_cqes_.data(), batch);
    unsigned seen = 0;
    for (unsigned i = 0; i < nr; ++i) {
      io_uring_cqe* cqe = batch_cqes_[i];
      IoRequest* req = static_cast<IoRequest*>(io_uring_cqe_get_data(cqe));
      bool hold_cqe_for_retry = false;
      if (req != nullptr && req->done_butex != nullptr) {
        if (cqe->res >= 0) {
          req->result = cqe->res;
          req->io_errno = 0;
        } else {
          req->result = -1;
          req->io_errno = -cqe->res;
        }

        static_cast<butil::atomic<int>*>(req->done_butex)
            ->store(1, butil::memory_order_release);
        const int wake_rc = bthread_butex_wake_within(wake_ctx, req->done_butex);
        if (wake_rc == -1 && errno == EAGAIN) {
          hold_cqe_for_retry = true;
        } else if (wake_rc == -1) {
          req->wake_errno = errno;
        }
      }

      if (hold_cqe_for_retry) {
        break;
      }
      ++seen;
      made_progress = true;
    }
    if (seen > 0) {
      io_uring_cq_advance(&ring_, seen);
    }
    return made_progress ? 1 : 0;
  }

 private:
  io_uring ring_{};
  std::vector<io_uring_cqe*> batch_cqes_;
  bool initialized_ = false;
};

DirectIoAlign ResolveDirectIoAlignFromFd(int fd) {
  DirectIoAlign align;
#if defined(SYS_statx) && defined(AT_EMPTY_PATH) && defined(STATX_DIOALIGN)
  struct statx stx;
  std::memset(&stx, 0, sizeof(stx));
  if (syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &stx) == 0) {
    if (stx.stx_dio_mem_align > 0 &&
        IsValidDirectIoAlign(stx.stx_dio_mem_align)) {
      align.mem_align = stx.stx_dio_mem_align;
    }
    if (stx.stx_dio_offset_align > 0 &&
        IsValidDirectIoAlign(stx.stx_dio_offset_align)) {
      align.offset_align = stx.stx_dio_offset_align;
    }
  }
#endif
  return align;
}

int ResolveFileOpenFlags() {
  int flags = FLAGS_file_open_flags;
#ifdef O_DIRECT
  if (FLAGS_file_direct_io) {
    flags |= O_DIRECT;
  }
#else
  if (FLAGS_file_direct_io) {
    LOG(ERROR) << "--file_direct_io is not supported: O_DIRECT is unavailable";
    return -1;
  }
#endif
  return flags;
}

int WorkerInit(void** worker_local, const bthread_active_task_ctx_t* ctx, void*) {
  if (worker_local == nullptr) {
    return 0;
  }
  *worker_local = nullptr;
  if (ctx == nullptr || ctx->tag != static_cast<bthread_tag_t>(FLAGS_iouring_tag)) {
    return 0;
  }
  std::unique_ptr<WorkerIoRing> ring(new (std::nothrow) WorkerIoRing());
  if (!ring) {
    LOG(ERROR) << "allocate WorkerIoRing failed on tag=" << FLAGS_iouring_tag;
    return 0;
  }
  tls_worker_ring = ring.get();
  *worker_local = ring.release();
  return 0;
}

void WorkerDestroy(void* worker_local, const bthread_active_task_ctx_t*, void*) {
  if (worker_local == nullptr) {
    return;
  }
  WorkerIoRing* ring = static_cast<WorkerIoRing*>(worker_local);
  if (tls_worker_ring == ring) {
    tls_worker_ring = nullptr;
  }
  delete ring;
}

int WorkerHarvest(void* worker_local, const bthread_active_task_ctx_t* ctx) {
  WorkerIoRing* ring = static_cast<WorkerIoRing*>(worker_local);
  if (ring == nullptr) {
    return 0;
  }
  return ring->Harvest(ctx);
}

int RegisterActiveTaskType() {
  bthread_active_task_type_t type;
  std::memset(&type, 0, sizeof(type));
  type.struct_size = sizeof(type);
  type.name = "read_bench_iouring";
  type.worker_init = WorkerInit;
  type.worker_destroy = WorkerDestroy;
  type.harvest = WorkerHarvest;
  return bthread_register_active_task_type(&type);
}

bool ValidateAndInitTagLayout() {
  if (FLAGS_monitor_tag < 0 || FLAGS_iouring_tag < 0 || FLAGS_pread_tag < 0) {
    LOG(ERROR) << "tags must be >= 0, monitor_tag=" << FLAGS_monitor_tag
               << " iouring_tag=" << FLAGS_iouring_tag
               << " pread_tag=" << FLAGS_pread_tag;
    return false;
  }
  if (FLAGS_monitor_tag == FLAGS_iouring_tag ||
      FLAGS_monitor_tag == FLAGS_pread_tag ||
      FLAGS_iouring_tag == FLAGS_pread_tag) {
    LOG(ERROR) << "tags must be distinct, monitor_tag=" << FLAGS_monitor_tag
               << " iouring_tag=" << FLAGS_iouring_tag
               << " pread_tag=" << FLAGS_pread_tag;
    return false;
  }
  if (FLAGS_port <= 0 || FLAGS_pread_port <= 0 || FLAGS_monitor_port <= 0) {
    LOG(ERROR) << "ports must be > 0, monitor_port=" << FLAGS_monitor_port
               << " iouring_port=" << FLAGS_port
               << " pread_port=" << FLAGS_pread_port;
    return false;
  }
  if (FLAGS_port == FLAGS_pread_port ||
      FLAGS_port == FLAGS_monitor_port ||
      FLAGS_pread_port == FLAGS_monitor_port) {
    LOG(ERROR) << "ports must be distinct, monitor_port=" << FLAGS_monitor_port
               << " iouring_port=" << FLAGS_port
               << " pread_port=" << FLAGS_pread_port;
    return false;
  }

  const int min_ntags = std::max(FLAGS_monitor_tag,
                         std::max(FLAGS_iouring_tag, FLAGS_pread_tag)) + 1;
  if (FLAGS_task_group_ntags < min_ntags) {
    FLAGS_task_group_ntags = min_ntags;
  }
  return true;
}

void SetReadFailure(const char* service_name,
                    const readbench::ReadRequest* req,
                    readbench::ReadResponse* resp,
                    int code,
                    const std::string& message) {
  resp->set_code(code);
  resp->set_message(message);
  const uint64_t offset = (req != nullptr) ? req->offset() : 0;
  const uint32_t len = (req != nullptr) ? req->len() : 0;
  LOG_EVERY_N(ERROR, kReadErrorLogEveryN)
      << service_name << " read failed"
      << " offset=" << offset
      << " len=" << len
      << " code=" << code
      << " message=" << message;
}

bool ValidateRequest(const char* service_name,
                     const readbench::ReadRequest* req,
                     readbench::ReadResponse* resp) {
  if (req->len() == 0 || req->len() > static_cast<uint32_t>(FLAGS_max_read_size)) {
    SetReadFailure(service_name, req, resp, EINVAL, "len out of range");
    return false;
  }
  if (g_use_direct_io) {
    const uint64_t align = static_cast<uint64_t>(g_direct_io_offset_align);
    if (align == 0 || (req->offset() % align) != 0 || (req->len() % align) != 0) {
      SetReadFailure(service_name, req, resp, EINVAL,
                     "direct io requires aligned offset/len, align=" +
                         std::to_string(g_direct_io_offset_align));
      return false;
    }
  }
  return true;
}

bool WaitForCompletion(IoRequest* req,
                       const readbench::ReadRequest* rpc_req,
                       readbench::ReadResponse* resp) {
  while (true) {
    // nullptr deadline means no timeout: wait indefinitely until wakeup.
    const int rc = bthread_butex_wait_local(req->done_butex, 0, nullptr);
    if (rc == 0) {
      break;
    }
    if (errno == EWOULDBLOCK) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    const int err = errno;
    SetReadFailure("io_uring", rpc_req, resp, err,
                   std::string("bthread_butex_wait_local failed: ") +
                       std::strerror(err));
    return false;
  }
  return true;
}

class IoUringReadServiceImpl : public readbench::IoUringReadService {
 public:
  void Read(::google::protobuf::RpcController*, const readbench::ReadRequest* req,
            readbench::ReadResponse* resp,
            ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);

    if (!ValidateRequest("io_uring", req, resp)) {
      return;
    }
    if (g_file_fd < 0) {
      SetReadFailure("io_uring", req, resp, EBADF, "file is not open");
      return;
    }
    if (tls_worker_ring == nullptr) {
      SetReadFailure("io_uring", req, resp, EINVAL,
                     "active-task worker io_uring is not initialized");
      return;
    }

    std::unique_ptr<IoRequest> io_req(new (std::nothrow) IoRequest());
    if (!io_req || io_req->done_butex == nullptr) {
      SetReadFailure("io_uring", req, resp, ENOMEM, "allocate io request failed");
      return;
    }

    const int queue_rc =
        tls_worker_ring->QueueRead(g_file_fd, req->offset(), req->len(), io_req.get());
    if (queue_rc != 0) {
      SetReadFailure("io_uring", req, resp, queue_rc,
                     std::string("queue io_uring read failed: ") +
                         std::strerror(queue_rc));
      return;
    }

    if (!WaitForCompletion(io_req.get(), req, resp)) {
      return;
    }
    if (io_req->wake_errno != 0) {
      SetReadFailure("io_uring", req, resp, io_req->wake_errno,
                     std::string("wake failed: ") +
                         std::strerror(io_req->wake_errno));
      return;
    }
    if (io_req->result < 0) {
      const int err = io_req->io_errno == 0 ? EIO : io_req->io_errno;
      SetReadFailure("io_uring", req, resp, err,
                     std::string("io_uring read failed: ") + std::strerror(err));
      return;
    }

    resp->set_code(0);
    resp->set_message("ok");
    resp->set_body(io_req->Data(), static_cast<size_t>(io_req->result));
  }
};

class PreadReadServiceImpl : public readbench::PreadReadService {
 public:
  void Read(::google::protobuf::RpcController*, const readbench::ReadRequest* req,
            readbench::ReadResponse* resp,
            ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);

    if (!ValidateRequest("pread", req, resp)) {
      return;
    }
    if (g_file_fd < 0) {
      SetReadFailure("pread", req, resp, EBADF, "file is not open");
      return;
    }

    char* pread_buf = nullptr;
    std::string fallback_buf;
    if (g_use_direct_io) {
      void* aligned_ptr = nullptr;
      const int alloc_rc = posix_memalign(&aligned_ptr, g_direct_io_mem_align, req->len());
      if (alloc_rc != 0) {
        SetReadFailure("pread", req, resp, alloc_rc,
                       std::string("allocate aligned buffer failed: ") +
                           std::strerror(alloc_rc));
        return;
      }
      pread_buf = static_cast<char*>(aligned_ptr);
    } else {
      fallback_buf.resize(req->len());
      pread_buf = fallback_buf.data();
    }

    const ssize_t n =
        pread(g_file_fd, pread_buf, req->len(), static_cast<off_t>(req->offset()));
    if (n < 0) {
      const int err = errno;
      if (g_use_direct_io) {
        std::free(pread_buf);
      }
      SetReadFailure("pread", req, resp, err,
                     std::string("pread failed: ") + std::strerror(err));
      return;
    }

    resp->set_code(0);
    resp->set_message("ok");
    resp->set_body(pread_buf, static_cast<size_t>(n));
    if (g_use_direct_io) {
      std::free(pread_buf);
    }
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  brpc::FLAGS_event_dispatcher_edisp_unsched = true;
  if (!ValidateAndInitTagLayout()) {
    return 1;
  }

  g_effective_file_open_flags = ResolveFileOpenFlags();
  if (g_effective_file_open_flags < 0) {
    return 1;
  }

  g_file_fd = open(FLAGS_file_path.c_str(), g_effective_file_open_flags);
  if (g_file_fd < 0) {
    LOG(ERROR) << "open failed, file_path=" << FLAGS_file_path
               << ", flags=" << g_effective_file_open_flags
               << ", errno=" << errno << "(" << std::strerror(errno) << ")";
    return 1;
  }
#ifdef O_DIRECT
  g_use_direct_io = ((g_effective_file_open_flags & O_DIRECT) != 0);
#else
  g_use_direct_io = false;
#endif
  if (g_use_direct_io) {
    const DirectIoAlign align = ResolveDirectIoAlignFromFd(g_file_fd);
    g_direct_io_mem_align = align.mem_align;
    g_direct_io_offset_align = align.offset_align;
  } else {
    g_direct_io_mem_align = 1;
    g_direct_io_offset_align = 1;
  }
  if (g_use_direct_io &&
      static_cast<uint64_t>(FLAGS_max_read_size) < g_direct_io_offset_align) {
    LOG(ERROR) << "invalid --max_read_size=" << FLAGS_max_read_size
               << ", it must be >= direct_io_offset_align="
               << g_direct_io_offset_align;
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }

  const int register_rc = RegisterActiveTaskType();
  if (register_rc != 0) {
    LOG(ERROR) << "register active task type failed, rc=" << register_rc;
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }

  brpc::Server monitor_server;
  brpc::Server iouring_server;
  brpc::Server pread_server;
  IoUringReadServiceImpl iouring_service;
  PreadReadServiceImpl pread_service;
  if (iouring_server.AddService(&iouring_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "add iouring service failed";
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }
  if (pread_server.AddService(&pread_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "add pread service failed";
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }

  brpc::ServerOptions monitor_options;
  monitor_options.num_threads = std::max(1, FLAGS_monitor_num_threads);
  monitor_options.bthread_tag = static_cast<bthread_tag_t>(FLAGS_monitor_tag);
  monitor_options.has_builtin_services = true;

  brpc::ServerOptions iouring_options;
  iouring_options.num_threads = std::max(1, FLAGS_num_threads);
  iouring_options.bthread_tag = static_cast<bthread_tag_t>(FLAGS_iouring_tag);
  iouring_options.has_builtin_services = false;

  brpc::ServerOptions pread_options;
  pread_options.num_threads = std::max(1, FLAGS_num_threads);
  pread_options.bthread_tag = static_cast<bthread_tag_t>(FLAGS_pread_tag);
  pread_options.has_builtin_services = false;

  if (monitor_server.Start(FLAGS_monitor_port, &monitor_options) != 0) {
    LOG(ERROR) << "monitor server start failed, port=" << FLAGS_monitor_port;
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }
  if (iouring_server.Start(FLAGS_port, &iouring_options) != 0) {
    LOG(ERROR) << "io_uring server start failed, port=" << FLAGS_port;
    monitor_server.Stop(0);
    monitor_server.Join();
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }
  if (pread_server.Start(FLAGS_pread_port, &pread_options) != 0) {
    LOG(ERROR) << "pread server start failed, port=" << FLAGS_pread_port;
    iouring_server.Stop(0);
    monitor_server.Stop(0);
    iouring_server.Join();
    monitor_server.Join();
    close(g_file_fd);
    g_file_fd = -1;
    return 1;
  }

  LOG(INFO) << "servers started:"
            << " monitor(port=" << FLAGS_monitor_port
            << ",tag=" << FLAGS_monitor_tag
            << ",threads=" << monitor_options.num_threads << ")"
            << " iouring(port=" << FLAGS_port
            << ",tag=" << FLAGS_iouring_tag
            << ",threads=" << iouring_options.num_threads << ")"
            << " pread(port=" << FLAGS_pread_port
            << ",tag=" << FLAGS_pread_tag
            << ",threads=" << pread_options.num_threads << ")"
            << " file_path=" << FLAGS_file_path
            << " file_open_flags=" << g_effective_file_open_flags
            << " direct_io=" << (g_use_direct_io ? "on" : "off")
            << " direct_io_mem_align=" << g_direct_io_mem_align
            << " direct_io_offset_align=" << g_direct_io_offset_align;
  LOG(INFO) << "thread_config monitor/iouring/pread="
            << monitor_options.num_threads << "/"
            << iouring_options.num_threads << "/"
            << pread_options.num_threads;
  LOG(INFO) << "active_task_config idle_wait_ns="
            << bthread::FLAGS_bthread_active_task_idle_wait_ns
            << " poll_every_nswitch="
            << bthread::FLAGS_bthread_active_task_poll_every_nswitch;

  monitor_server.RunUntilAskedToQuit();

  pread_server.Stop(0);
  iouring_server.Stop(0);
  monitor_server.Stop(0);
  pread_server.Join();
  iouring_server.Join();
  monitor_server.Join();

  close(g_file_fd);
  g_file_fd = -1;
  return 0;
}
