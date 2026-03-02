#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <bvar/bvar.h>
#include <bthread/butex.h>
#include <bthread/bthread.h>
#include <bthread/unstable.h>
#include <butil/logging.h>
#include <gflags/gflags.h>

#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

#include <brpc/server.h>

#include "file_read.pb.h"

DECLARE_int32(task_group_ntags);

DEFINE_int32(port, 8002, "Server listen port");
DEFINE_int32(blocking_port, 8042, "Blocking-read server listen port");
DEFINE_int32(monitor_port, 8003, "Builtin monitor service port");
DEFINE_int32(read_tag, 1, "bthread tag for file read requests");
DEFINE_int32(blocking_tag, 2, "bthread tag for blocking file read requests");
DEFINE_int32(monitor_tag, 0, "bthread tag for monitor builtin services");
DEFINE_int32(read_num_threads, 12,
             "pthread workers hint for read server (default: 12)");
DEFINE_int32(blocking_num_threads, 12,
             "pthread workers hint for blocking read server; <0 means follow read_num_threads");
DEFINE_int32(monitor_num_threads, 4,
             "pthread workers hint for monitor server (default: 4)");
DEFINE_string(file_path, "/tmp/iouring-read-default.txt",
              "Default file path when request.path is empty");
DEFINE_int32(max_read_len, 1 << 20, "Maximum readable bytes per request");
DEFINE_int32(read_len_bytes, 32 * 1024,
             "Required request len in bytes");
DEFINE_int32(read_timeout_ms, 2000, "RPC wait_local timeout in milliseconds");
DEFINE_int32(direct_io_align, 4096, "Alignment bytes for O_DIRECT reads");
DEFINE_int32(max_aligned_read_len, 4 << 20,
             "Maximum aligned bytes read per request for O_DIRECT paths");
DEFINE_int32(iouring_batch_cqe_max, 128,
             "Max CQEs to peek in one harvest round");
DEFINE_bool(iouring_setup_coop_taskrun, true,
            "Enable IORING_SETUP_COOP_TASKRUN when supported");
DEFINE_bool(iouring_setup_single_issuer, true,
            "Enable IORING_SETUP_SINGLE_ISSUER when supported");

namespace {

constexpr unsigned kRingQueueDepth = 512;
constexpr uint64_t kReqCtxBufferSizeAlign = 32 * 1024;

bvar::Adder<int64_t> g_file_read_ops_total("file_read_ops_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_file_read_iops("file_read_iops",
                                                       &g_file_read_ops_total);
bvar::LatencyRecorder g_file_read_iouring_total_us("file_read_iouring_total_us");
bvar::LatencyRecorder g_file_read_iouring_queue_us("file_read_iouring_queue_us");
bvar::LatencyRecorder g_file_read_iouring_wait_us("file_read_iouring_wait_us");
bvar::LatencyRecorder g_file_read_iouring_copy_us("file_read_iouring_copy_us");
bvar::Adder<int64_t> g_iouring_harvest_cqe_total("file_read_iouring_harvest_cqe_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_iouring_harvest_cqe_qps(
    "file_read_iouring_harvest_cqe_qps", &g_iouring_harvest_cqe_total);
bvar::Adder<int64_t> g_iouring_harvest_wake_eagain_total(
    "file_read_iouring_harvest_wake_eagain_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_iouring_harvest_wake_eagain_qps(
    "file_read_iouring_harvest_wake_eagain_qps", &g_iouring_harvest_wake_eagain_total);
bvar::Adder<int64_t> g_iouring_harvest_wake_error_total(
    "file_read_iouring_harvest_wake_error_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_iouring_harvest_wake_error_qps(
    "file_read_iouring_harvest_wake_error_qps", &g_iouring_harvest_wake_error_total);
bvar::Adder<int64_t> g_iouring_wait_orphan_total("file_read_iouring_wait_orphan_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_iouring_wait_orphan_qps(
    "file_read_iouring_wait_orphan_qps", &g_iouring_wait_orphan_total);
bvar::Adder<int64_t> g_iouring_orphan_recycled_total(
    "file_read_iouring_orphan_recycled_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_iouring_orphan_recycled_qps(
    "file_read_iouring_orphan_recycled_qps", &g_iouring_orphan_recycled_total);
bvar::Adder<int64_t> g_file_read_blocking_ops_total("file_read_blocking_ops_total");
bvar::PerSecond<bvar::Adder<int64_t>> g_file_read_blocking_iops(
    "file_read_blocking_iops", &g_file_read_blocking_ops_total);
bvar::LatencyRecorder g_file_read_blocking_total_us("file_read_blocking_total_us");
bvar::LatencyRecorder g_file_read_blocking_pread_us("file_read_blocking_pread_us");
int g_shared_direct_fd = -1;

inline int64_t MonoNowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct ScopedLatencyUs {
  explicit ScopedLatencyUs(bvar::LatencyRecorder* rec) : rec_(rec), begin_ns_(MonoNowNs()) {}
  ~ScopedLatencyUs() {
    if (rec_ != nullptr) {
      (*rec_) << ((MonoNowNs() - begin_ns_) / 1000);
    }
  }

 private:
  bvar::LatencyRecorder* rec_ = nullptr;
  int64_t begin_ns_ = 0;
};

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

  ReqCtx(const ReqCtx&) = delete;
  ReqCtx& operator=(const ReqCtx&) = delete;
  ReqCtx(ReqCtx&&) = delete;
  ReqCtx& operator=(ReqCtx&&) = delete;

  int EnsureAlignedBuffer(size_t alignment, size_t length) {
    if (length == 0) {
      return EINVAL;
    }
    const size_t growth_unit =
        FLAGS_read_len_bytes > 0 ? static_cast<size_t>(FLAGS_read_len_bytes) : size_t{1};
    const size_t required_capacity =
        ((length + growth_unit - 1) / growth_unit) * growth_unit;
    if (aligned_buffer == nullptr || buffer_capacity < required_capacity) {
      if (aligned_buffer != nullptr) {
        free(aligned_buffer);
        aligned_buffer = nullptr;
      }
      if (posix_memalign(&aligned_buffer, alignment, required_capacity) != 0) {
        aligned_buffer = nullptr;
        aligned_len = 0;
        buffer_capacity = 0;
        return ENOMEM;
      }
      buffer_capacity = required_capacity;
    }
    aligned_len = length;
    return 0;
  }

  void ResetForNewRequest() {
    request_len = 0;
    prefix_skip = 0;
    io_result = 0;
    io_errno = 0;
    wake_errno = 0;
    orphaned.store(false, std::memory_order_release);
    returned_to_pool.store(false, std::memory_order_release);
    if (done_butex != nullptr) {
      static_cast<butil::atomic<int>*>(done_butex)->store(0,
                                                          butil::memory_order_release);
    }
  }

  void* done_butex = nullptr;
  void* aligned_buffer = nullptr;
  size_t aligned_len = 0;
  size_t buffer_capacity = 0;
  size_t prefix_skip = 0;
  size_t request_len = 0;
  ssize_t io_result = 0;
  int io_errno = 0;
  int wake_errno = 0;
  std::atomic<bool> orphaned{false};
  std::atomic<bool> returned_to_pool{true};
  ReqCtx* next_free = nullptr;
};

class WorkerIoState {
 public:
  WorkerIoState() : initialized_(false) { memset(&ring_, 0, sizeof(ring_)); }

  ~WorkerIoState() { Destroy(); }

  int Init() {
    if (initialized_) {
      return 0;
    }
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
    int rc = io_uring_queue_init_params(kRingQueueDepth, &ring_, &p);
    if (rc < 0) {
      // Fall back to baseline setup when tuned flags are not available in the runtime.
      rc = io_uring_queue_init(kRingQueueDepth, &ring_, 0);
      if (rc < 0) {
        return -rc;
      }
    }
    req_ctx_pool_.reserve(kRingQueueDepth);
    free_head_ = nullptr;
    for (unsigned i = 0; i < kRingQueueDepth; ++i) {
      req_ctx_pool_.emplace_back(new ReqCtx());
      ReqCtx* ctx = req_ctx_pool_.back().get();
      if (ctx->done_butex == nullptr) {
        return ENOMEM;
      }
      ctx->returned_to_pool.store(true, std::memory_order_release);
      ctx->next_free = free_head_;
      free_head_ = ctx;
    }
    initialized_ = true;
    return 0;
  }

  void Destroy() {
    if (!initialized_) {
      return;
    }
    free_head_ = nullptr;
    req_ctx_pool_.clear();
    io_uring_queue_exit(&ring_);
    initialized_ = false;
  }

  ReqCtx* AcquireReqCtx() {
    if (free_head_ == nullptr) {
      return nullptr;
    }
    ReqCtx* ctx = free_head_;
    free_head_ = ctx->next_free;
    ctx->next_free = nullptr;
    ctx->ResetForNewRequest();
    return ctx;
  }

  void ReleaseReqCtx(ReqCtx* req) {
    if (req == nullptr) {
      return;
    }
    bool expected = false;
    if (!req->returned_to_pool.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
      return;
    }
    req->next_free = free_head_;
    free_head_ = req;
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
      g_iouring_harvest_cqe_total << n;
      unsigned advanced = 0;
      bool blocked_on_wake_eagain = false;
      std::vector<ReqCtx*> recycled_reqs;
      recycled_reqs.reserve(n);
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
          const bool orphaned = req->orphaned.load(std::memory_order_acquire);
          if (!orphaned) {
            const int wake_rc = bthread_butex_wake_within(wake_ctx, req->done_butex);
            if (wake_rc == 1 || wake_rc == 0) {
              if (wake_rc == 1) {
                made_progress = true;
              }
            } else if (wake_rc == -1 && errno == EAGAIN) {
              g_iouring_harvest_wake_eagain_total << 1;
              // pin_rq is temporarily full: keep this CQE for next harvest retry.
              blocked_on_wake_eagain = true;
              break;
            } else {
              g_iouring_harvest_wake_error_total << 1;
              req->wake_errno = errno;
            }
          }
          static_cast<butil::atomic<int>*>(req->done_butex)
              ->store(1, butil::memory_order_release);
          if (orphaned) {
            recycled_reqs.push_back(req);
          }
        }
        ++advanced;
      }
      if (advanced > 0) {
        io_uring_cq_advance(&ring_, advanced);
        for (ReqCtx* req : recycled_reqs) {
          ReleaseReqCtx(req);
          g_iouring_orphan_recycled_total << 1;
        }
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
  std::vector<std::unique_ptr<ReqCtx>> req_ctx_pool_;
  ReqCtx* free_head_ = nullptr;
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
    ScopedLatencyUs total_lat(&g_file_read_iouring_total_us);

    if (req->offset() < 0) {
      return SetInvalidArgument(resp, "offset must be non-negative");
    }
    if (req->len() != FLAGS_read_len_bytes) {
      return SetInvalidArgument(resp, "len must equal server --read_len_bytes");
    }
    if (req->offset() >
        (std::numeric_limits<int64_t>::max() - static_cast<int64_t>(FLAGS_read_len_bytes))) {
      return SetInvalidArgument(resp, "offset + len overflow");
    }

    const size_t alignment = static_cast<size_t>(FLAGS_direct_io_align);
    if (!IsPowerOfTwo(alignment) || alignment < 512) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("direct_io_align must be power-of-two and >= 512");
      resp->clear_body();
      return;
    }

    if (!req->path().empty() && req->path() != FLAGS_file_path) {
      return SetInvalidArgument(resp, "request.path override is disabled");
    }
    if (g_shared_direct_fd < 0) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("shared direct fd unavailable");
      resp->clear_body();
      return;
    }
    const int fd = g_shared_direct_fd;

    const uint64_t request_offset = static_cast<uint64_t>(req->offset());
    const uint64_t request_len = static_cast<uint64_t>(FLAGS_read_len_bytes);
    const uint64_t request_end = request_offset + request_len;
    const uint64_t aligned_offset = AlignDown(request_offset, alignment);
    const uint64_t aligned_end = AlignUp(request_end, alignment);
    const uint64_t aligned_len = aligned_end - aligned_offset;
    if (aligned_len == 0 || aligned_len > static_cast<uint64_t>(FLAGS_max_aligned_read_len)) {
      return SetInvalidArgument(resp, "aligned read length out of range");
    }

    if (!g_worker_io) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("worker io state unavailable");
      return;
    }
    ReqCtx* request_ctx = g_worker_io->AcquireReqCtx();
    if (request_ctx == nullptr) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("req_ctx pool exhausted");
      return;
    }
    struct ReqCtxReleaser {
      WorkerIoState* owner = nullptr;
      ReqCtx* req = nullptr;
      ~ReqCtxReleaser() {
        if (owner != nullptr && req != nullptr) {
          owner->ReleaseReqCtx(req);
        }
      }
    } releaser{g_worker_io, request_ctx};

    request_ctx->request_len = static_cast<size_t>(request_len);
    request_ctx->prefix_skip = static_cast<size_t>(request_offset - aligned_offset);
    const int prep_rc =
        request_ctx->EnsureAlignedBuffer(alignment, static_cast<size_t>(aligned_len));
    if (prep_rc != 0) {
      return SetIoError(resp, prep_rc, "allocate aligned buffer failed");
    }
    const int64_t queue_begin_ns = MonoNowNs();
    const int submit_rc =
        g_worker_io->QueueRead(fd, request_ctx, static_cast<off_t>(aligned_offset));
    g_file_read_iouring_queue_us << ((MonoNowNs() - queue_begin_ns) / 1000);
    if (submit_rc != 0) {
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

    const int64_t wait_begin_ns = MonoNowNs();
    const int wait_rc = bthread_butex_wait_local(request_ctx->done_butex, 0, &ts);
    g_file_read_iouring_wait_us << ((MonoNowNs() - wait_begin_ns) / 1000);
    const int butex_value =
        static_cast<butil::atomic<int>*>(request_ctx->done_butex)
            ->load(butil::memory_order_acquire);
    if (wait_rc != 0 && errno != EWOULDBLOCK && butex_value == 0) {
      const int saved_errno = errno;
      // CQE may still arrive later. Keep this ReqCtx out of freelist to avoid
      // stale CQE writing into a reused context.
      request_ctx->orphaned.store(true, std::memory_order_release);
      releaser.req = nullptr;
      g_iouring_wait_orphan_total << 1;
      // Race fix: CQE may have just completed and been advanced before orphaned
      // became visible to harvest. Opportunistically reclaim here.
      const int done_after_orphan =
          static_cast<butil::atomic<int>*>(request_ctx->done_butex)
              ->load(butil::memory_order_acquire);
      if (done_after_orphan == 1) {
        g_worker_io->ReleaseReqCtx(request_ctx);
      }
      if (saved_errno == ETIMEDOUT) {
        resp->set_code(iouring_file_read::FILE_READ_TIMEOUT);
        resp->set_message("wait_local timeout");
      } else {
        SetIoError(resp, saved_errno, "wait_local failed");
      }
      return;
    }

    if (request_ctx->wake_errno != 0) {
      SetIoError(resp, request_ctx->wake_errno, "wake_within failed");
      return;
    }
    if (request_ctx->io_result < 0) {
      SetIoError(resp, request_ctx->io_errno, "read failed");
      return;
    }

    g_file_read_ops_total << 1;
    resp->set_code(iouring_file_read::FILE_READ_OK);
    const int64_t copy_begin_ns = MonoNowNs();
    if (request_ctx->io_result <= static_cast<ssize_t>(request_ctx->prefix_skip)) {
      resp->clear_body();
    } else {
      const size_t readable_after_skip =
          static_cast<size_t>(request_ctx->io_result) - request_ctx->prefix_skip;
      const size_t body_len = std::min(readable_after_skip, request_ctx->request_len);
      const char* body_ptr =
          static_cast<const char*>(request_ctx->aligned_buffer) + request_ctx->prefix_skip;
      resp->set_body(body_ptr, body_len);
    }
    g_file_read_iouring_copy_us << ((MonoNowNs() - copy_begin_ns) / 1000);
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

class BlockingFileReadServiceImpl : public iouring_file_read::BlockingFileReadService {
 public:
  void Read(::google::protobuf::RpcController* cntl_base,
            const iouring_file_read::FileReadRequest* req,
            iouring_file_read::FileReadResponse* resp,
            ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    ScopedLatencyUs total_lat(&g_file_read_blocking_total_us);

    if (req->offset() < 0) {
      return SetInvalidArgument(resp, "offset must be non-negative");
    }
    if (req->len() != FLAGS_read_len_bytes) {
      return SetInvalidArgument(resp, "len must equal server --read_len_bytes");
    }
    if (req->offset() >
        (std::numeric_limits<int64_t>::max() - static_cast<int64_t>(FLAGS_read_len_bytes))) {
      return SetInvalidArgument(resp, "offset + len overflow");
    }

    const size_t alignment = static_cast<size_t>(FLAGS_direct_io_align);
    if (!IsPowerOfTwo(alignment) || alignment < 512) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("direct_io_align must be power-of-two and >= 512");
      resp->clear_body();
      return;
    }

    if (!req->path().empty() && req->path() != FLAGS_file_path) {
      return SetInvalidArgument(resp, "request.path override is disabled");
    }
    if (g_shared_direct_fd < 0) {
      resp->set_code(iouring_file_read::FILE_READ_INTERNAL);
      resp->set_message("shared direct fd unavailable");
      resp->clear_body();
      return;
    }
    const int fd = g_shared_direct_fd;

    const uint64_t request_offset = static_cast<uint64_t>(req->offset());
    const uint64_t request_len = static_cast<uint64_t>(FLAGS_read_len_bytes);
    const uint64_t request_end = request_offset + request_len;
    const uint64_t aligned_offset = AlignDown(request_offset, alignment);
    const uint64_t aligned_end = AlignUp(request_end, alignment);
    const uint64_t aligned_len = aligned_end - aligned_offset;
    if (aligned_len == 0 || aligned_len > static_cast<uint64_t>(FLAGS_max_aligned_read_len)) {
      return SetInvalidArgument(resp, "aligned read length out of range");
    }

    void* aligned_buffer = nullptr;
    if (posix_memalign(&aligned_buffer, alignment, static_cast<size_t>(aligned_len)) != 0) {
      return SetIoError(resp, ENOMEM, "allocate aligned buffer failed");
    }
    memset(aligned_buffer, 0, static_cast<size_t>(aligned_len));

    const int64_t pread_begin_ns = MonoNowNs();
    const ssize_t n = pread(fd, aligned_buffer, static_cast<size_t>(aligned_len),
                            static_cast<off_t>(aligned_offset));
    g_file_read_blocking_pread_us << ((MonoNowNs() - pread_begin_ns) / 1000);
    const int saved_errno = errno;

    if (n < 0) {
      free(aligned_buffer);
      return SetIoError(resp, saved_errno, "pread failed");
    }

    std::string body;
    const size_t prefix_skip = static_cast<size_t>(request_offset - aligned_offset);
    if (n > static_cast<ssize_t>(prefix_skip)) {
      const size_t readable_after_skip = static_cast<size_t>(n) - prefix_skip;
      const size_t body_len = std::min(readable_after_skip, static_cast<size_t>(request_len));
      const char* body_ptr = static_cast<const char*>(aligned_buffer) + prefix_skip;
      body.assign(body_ptr, body_len);
    }
    free(aligned_buffer);

    g_file_read_blocking_ops_total << 1;
    resp->set_code(iouring_file_read::FILE_READ_OK);
    resp->set_message("ok");
    resp->set_body(body);
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
  if (FLAGS_read_len_bytes <= 0) {
    LOG(ERROR) << "read_len_bytes must be positive";
    return 1;
  }
  if (FLAGS_read_len_bytes > FLAGS_max_read_len) {
    LOG(ERROR) << "read_len_bytes(" << FLAGS_read_len_bytes
               << ") exceeds max_read_len(" << FLAGS_max_read_len << ")";
    return 1;
  }
  g_shared_direct_fd = open(FLAGS_file_path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
  if (g_shared_direct_fd < 0) {
    LOG(ERROR) << "Fail to open file_path with O_DIRECT: " << FLAGS_file_path
               << ", errno=" << errno << " (" << strerror(errno) << ")";
    return 1;
  }
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
  if (FLAGS_blocking_num_threads < 0) {
    FLAGS_blocking_num_threads = FLAGS_read_num_threads;
  }
  if (FLAGS_blocking_num_threads < kMinTagConcurrency) {
    LOG(WARNING) << "blocking_num_threads(" << FLAGS_blocking_num_threads
                 << ") < " << kMinTagConcurrency << ", bump to "
                 << kMinTagConcurrency;
    FLAGS_blocking_num_threads = kMinTagConcurrency;
  }
  const int needed_ntags =
      std::max(std::max(FLAGS_read_tag, FLAGS_monitor_tag), FLAGS_blocking_tag) + 1;
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
  brpc::Server blocking_server;
  brpc::Server monitor_server;
  FileReadServiceImpl svc;
  BlockingFileReadServiceImpl blocking_svc;
  if (read_server.AddService(&svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "Fail to add service";
    return -1;
  }
  if (blocking_server.AddService(&blocking_svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "Fail to add blocking service";
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

  brpc::ServerOptions blocking_options;
  blocking_options.bthread_tag = FLAGS_blocking_tag;
  blocking_options.has_builtin_services = false;
  int blocking_threads = FLAGS_blocking_num_threads;
  const int current_blocking_threads =
      bthread_getconcurrency_by_tag(static_cast<bthread_tag_t>(FLAGS_blocking_tag));
  if (current_blocking_threads > blocking_threads) {
    LOG(WARNING) << "blocking tag " << FLAGS_blocking_tag
                 << " current concurrency is " << current_blocking_threads
                 << ", cannot decrease to " << blocking_threads
                 << ", use " << current_blocking_threads;
    blocking_threads = current_blocking_threads;
  }
  blocking_options.num_threads = blocking_threads;
  if (blocking_server.Start(FLAGS_blocking_port, &blocking_options) != 0) {
    LOG(ERROR) << "Fail to start blocking read server";
    return -1;
  }

  read_server.RunUntilAskedToQuit();
  monitor_server.RunUntilAskedToQuit();
  blocking_server.RunUntilAskedToQuit();
  close(g_shared_direct_fd);
  g_shared_direct_fd = -1;
  return 0;
}
