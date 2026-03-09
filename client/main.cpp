#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <brpc/channel.h>
#include <brpc/event_dispatcher.h>
#include <gflags/gflags.h>

#include "file_read.pb.h"

DEFINE_string(iouring_server_addr, "127.0.0.1:8002",
              "io_uring server address host:port");
DEFINE_string(pread_server_addr, "127.0.0.1:8003",
              "pread server address host:port");
DEFINE_string(service, "io_uring", "target service: io_uring|pread");
DEFINE_int32(concurrency, 8, "concurrent client threads per endpoint");
DEFINE_int32(duration_s, 10, "benchmark duration in seconds per endpoint");
DEFINE_int32(read_size, 4096, "bytes per read request");
DEFINE_uint64(request_align, 1,
              "align request offset and read_size to N bytes; 1 disables");
DEFINE_uint64(file_size, 64ull * 1024 * 1024, "target file size in bytes");
DEFINE_uint64(seed, 0, "random seed; 0 means using current system time");

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr uint64_t kIoUringSeedSalt = 0x11111111ull;
constexpr uint64_t kPreadSeedSalt = 0x22222222ull;

enum class TargetService {
  KNone,
  kIoUring,
  kPread,
};

struct LatencyStats {
  uint32_t avg_us = 0;
  uint32_t p50_us = 0;
  uint32_t p99_us = 0;
  uint32_t p999_us = 0;
  uint32_t p9999_us = 0;
};

struct BenchResult {
  std::string name;
  uint64_t ok = 0;
  uint64_t fail = 0;
  uint64_t bytes = 0;
  double seconds = 0;
  double qps = 0;
  double mbps = 0;
  LatencyStats latency;
};

struct RuntimeStats {
  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> fail{0};
  std::atomic<uint64_t> bytes{0};
  std::mutex latency_mu;
  std::vector<uint32_t> second_latency_us;
  std::vector<uint32_t> all_latency_us;
};

// Fast non-cryptographic PRNG for request generation.
class SplitMix64 {
 public:
  explicit SplitMix64(uint64_t seed) : state_(seed) {
    if (state_ == 0) {
      state_ = 0x9e3779b97f4a7c15ull;
    }
  }

  uint64_t Next() {
    uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }

 private:
  uint64_t state_;
};

inline uint64_t FastRange64(uint64_t random, uint64_t range) {
  if (range == 0) {
    return random;
  }
#if defined(__SIZEOF_INT128__)
  return static_cast<uint64_t>(
      (static_cast<unsigned __int128>(random) * range) >> 64);
#else
  return random % range;
#endif
}

bool ParseTargetService(const std::string& text, TargetService* out) {
  if (out == nullptr) {
    return false;
  }
  if (text == "io_uring") {
    *out = TargetService::kIoUring;
    return true;
  }
  if (text == "pread") {
    *out = TargetService::kPread;
    return true;
  }
  return false;
}

bool ValidateFlags() {
  if (FLAGS_concurrency <= 0 || FLAGS_duration_s <= 0 || FLAGS_read_size <= 0) {
    std::cerr << "invalid flags: concurrency/duration_s/read_size must be > 0\n";
    return false;
  }
  if (FLAGS_request_align == 0) {
    std::cerr << "invalid flags: request_align must be >= 1\n";
    return false;
  }
  if (FLAGS_request_align > 1 &&
      (static_cast<uint64_t>(FLAGS_read_size) % FLAGS_request_align) != 0) {
    std::cerr << "invalid flags: read_size must be a multiple of request_align\n";
    return false;
  }
  return true;
}

uint64_t ResolveSeed() {
  if (FLAGS_seed != 0) {
    return FLAGS_seed;
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

uint64_t RandomOffset(SplitMix64* rng) {
  const uint64_t read_size = static_cast<uint64_t>(FLAGS_read_size);
  if (FLAGS_file_size <= read_size) {
    return 0;
  }
  const uint64_t max_offset = FLAGS_file_size - read_size;
  if (FLAGS_request_align <= 1) {
    return FastRange64(rng->Next(), max_offset + 1);
  }
  const uint64_t slots = max_offset / FLAGS_request_align;
  return FastRange64(rng->Next(), slots + 1) * FLAGS_request_align;
}

void BuildReadRequest(SplitMix64* rng, readbench::ReadRequest* req) {
  req->set_len(static_cast<uint32_t>(FLAGS_read_size));
  req->set_offset(RandomOffset(rng));
}

bool IsReadSuccess(const brpc::Controller& cntl,
                   const readbench::ReadRequest& req,
                   const readbench::ReadResponse& resp) {
  return !cntl.Failed() &&
         resp.code() == 0 &&
         resp.body().size() == static_cast<size_t>(req.len());
}

uint32_t Percentile(const std::vector<uint32_t>& sorted, double ratio) {
  if (sorted.empty()) {
    return 0;
  }
  const size_t idx =
      static_cast<size_t>(ratio * static_cast<double>(sorted.size() - 1));
  return sorted[idx];
}

LatencyStats SummarizeLatency(std::vector<uint32_t>* latencies_us) {
  LatencyStats stats;
  if (latencies_us == nullptr || latencies_us->empty()) {
    return stats;
  }

  std::sort(latencies_us->begin(), latencies_us->end());
  const uint64_t sum_us =
      std::accumulate(latencies_us->begin(), latencies_us->end(), uint64_t{0});
  stats.avg_us = static_cast<uint32_t>(sum_us / latencies_us->size());
  stats.p50_us = Percentile(*latencies_us, 0.50);
  stats.p99_us = Percentile(*latencies_us, 0.99);
  stats.p999_us = Percentile(*latencies_us, 0.999);
  stats.p9999_us = Percentile(*latencies_us, 0.9999);
  return stats;
}

void PrintSecondResult(const std::string& name,
                       int second,
                       uint64_t ok,
                       uint64_t fail,
                       uint64_t bytes,
                       const LatencyStats& latency) {
  const uint64_t done = ok + fail;
  const double qps = static_cast<double>(done);
  const double mbps = static_cast<double>(bytes) / (1024.0 * 1024.0);

  std::cout << "[" << name << "]"
            << " sec=" << second
            << " ok=" << ok
            << " fail=" << fail
            << " qps=" << std::fixed << std::setprecision(2) << qps
            << " mbps=" << mbps
            << " avg_us=" << latency.avg_us
            << " p50_us=" << latency.p50_us
            << " p99_us=" << latency.p99_us
            << " p999_us=" << latency.p999_us
            << " p9999_us=" << latency.p9999_us << '\n';
}

void PrintFinalResult(const BenchResult& result) {
  std::cout << std::left << std::setw(10) << result.name
            << " total_ok=" << result.ok
            << " total_fail=" << result.fail
            << " qps=" << std::fixed << std::setprecision(2) << result.qps
            << " mbps=" << result.mbps
            << " avg_us=" << result.latency.avg_us
            << " p50_us=" << result.latency.p50_us
            << " p99_us=" << result.latency.p99_us
            << " p999_us=" << result.latency.p999_us
            << " p9999_us=" << result.latency.p9999_us << '\n';
}

brpc::ChannelOptions BuildChannelOptions() {
  brpc::ChannelOptions options;
  options.protocol = "baidu_std";
  options.timeout_ms = -1;          // wait indefinitely for RPC response
  options.connect_timeout_ms = -1;  // wait indefinitely for connection
  options.max_retry = 0;
  return options;
}

template <typename StubType>
BenchResult RunTimedBench(const std::string& name, StubType* stub, uint64_t run_seed) {
  BenchResult result;
  result.name = name;
  RuntimeStats stats;

  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(FLAGS_concurrency));

  const auto begin = SteadyClock::now();
  for (int i = 0; i < FLAGS_concurrency; ++i) {
    workers.emplace_back([&, i]() {
      SplitMix64 rng(run_seed ^ (0x9e3779b97f4a7c15ull *
                                 static_cast<uint64_t>(i + 1)));
      while (!stop.load(std::memory_order_relaxed)) {
        readbench::ReadRequest req;
        BuildReadRequest(&rng, &req);

        readbench::ReadResponse resp;
        brpc::Controller cntl;
        const auto t0 = SteadyClock::now();
        stub->Read(&cntl, &req, &resp, nullptr);
        const auto t1 = SteadyClock::now();

        if (IsReadSuccess(cntl, req, resp)) {
          const uint32_t latency_us = static_cast<uint32_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
          stats.ok.fetch_add(1, std::memory_order_relaxed);
          stats.bytes.fetch_add(static_cast<uint64_t>(resp.body().size()),
                                std::memory_order_relaxed);
          std::lock_guard<std::mutex> guard(stats.latency_mu);
          stats.second_latency_us.push_back(latency_us);
          stats.all_latency_us.push_back(latency_us);
        } else {
          stats.fail.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  uint64_t prev_ok = 0;
  uint64_t prev_fail = 0;
  uint64_t prev_bytes = 0;
  auto next_tick = begin + std::chrono::seconds(1);
  for (int second = 1; second <= FLAGS_duration_s; ++second) {
    std::this_thread::sleep_until(next_tick);
    next_tick += std::chrono::seconds(1);

    const uint64_t curr_ok = stats.ok.load(std::memory_order_relaxed);
    const uint64_t curr_fail = stats.fail.load(std::memory_order_relaxed);
    const uint64_t curr_bytes = stats.bytes.load(std::memory_order_relaxed);
    const uint64_t delta_ok = curr_ok - prev_ok;
    const uint64_t delta_fail = curr_fail - prev_fail;
    const uint64_t delta_bytes = curr_bytes - prev_bytes;
    prev_ok = curr_ok;
    prev_fail = curr_fail;
    prev_bytes = curr_bytes;

    std::vector<uint32_t> second_latency_us;
    {
      std::lock_guard<std::mutex> guard(stats.latency_mu);
      second_latency_us.swap(stats.second_latency_us);
    }
    PrintSecondResult(name, second, delta_ok, delta_fail, delta_bytes,
                      SummarizeLatency(&second_latency_us));
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& worker : workers) {
    worker.join();
  }
  const auto end = SteadyClock::now();

  result.ok = stats.ok.load(std::memory_order_relaxed);
  result.fail = stats.fail.load(std::memory_order_relaxed);
  result.bytes = stats.bytes.load(std::memory_order_relaxed);
  result.seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
  const uint64_t done = result.ok + result.fail;
  if (result.seconds > 0) {
    result.qps = static_cast<double>(done) / result.seconds;
    result.mbps =
        (static_cast<double>(result.bytes) / (1024.0 * 1024.0)) / result.seconds;
  }
  result.latency = SummarizeLatency(&stats.all_latency_us);
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  brpc::FLAGS_event_dispatcher_edisp_unsched = true;

  TargetService target = TargetService::KNone;
  if (!ParseTargetService(FLAGS_service, &target)) {
    std::cerr << "invalid flags: --service must be io_uring|pread\n";
    return 1;
  }
  if (!ValidateFlags()) {
    return 1;
  }

  const bool run_iouring =
      (target == TargetService::kIoUring);
  const bool run_pread =
      (target == TargetService::kPread);

  brpc::Channel iouring_channel;
  brpc::Channel pread_channel;
  const brpc::ChannelOptions options = BuildChannelOptions();
  if (run_iouring &&
      iouring_channel.Init(FLAGS_iouring_server_addr.c_str(), nullptr, &options) != 0) {
    std::cerr << "failed to init io_uring channel, addr="
              << FLAGS_iouring_server_addr << '\n';
    return 1;
  }
  if (run_pread &&
      pread_channel.Init(FLAGS_pread_server_addr.c_str(), nullptr, &options) != 0) {
    std::cerr << "failed to init pread channel, addr="
              << FLAGS_pread_server_addr << '\n';
    return 1;
  }

  std::cout << "no-timeout mode: channel timeout_ms=-1, connect_timeout_ms=-1\n";
  std::cout << "target service: " << FLAGS_service << '\n';
  const uint64_t seed = ResolveSeed();
  std::cout << "seed: " << seed << '\n';
  std::cout << "timed benchmark: duration_s=" << FLAGS_duration_s
            << ", per-second metrics enabled\n";

  BenchResult iouring;
  BenchResult pread;
  if (run_iouring) {
    readbench::IoUringReadService_Stub iouring_stub(&iouring_channel);
    iouring = RunTimedBench("io_uring", &iouring_stub, seed ^ kIoUringSeedSalt);
  }
  if (run_pread) {
    readbench::PreadReadService_Stub pread_stub(&pread_channel);
    pread = RunTimedBench("pread", &pread_stub, seed ^ kPreadSeedSalt);
  }

  std::cout << "=== final ===\n";
  if (run_iouring) {
    PrintFinalResult(iouring);
  }
  if (run_pread) {
    PrintFinalResult(pread);
  }

  if (run_iouring && run_pread && pread.qps > 0) {
    std::cout << "speedup_qps(io_uring/pread)=" << std::fixed << std::setprecision(3)
              << (iouring.qps / pread.qps) << '\n';
  }
  if (run_iouring && run_pread &&
      pread.latency.p99_us > 0 && iouring.latency.p99_us > 0) {
    std::cout << "speedup_p99_latency(pread/io_uring)=" << std::fixed
              << std::setprecision(3)
              << (static_cast<double>(pread.latency.p99_us) / iouring.latency.p99_us)
              << '\n';
  }
  return 0;
}
