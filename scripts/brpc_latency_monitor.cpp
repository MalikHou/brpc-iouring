#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>

#include "file_read.pb.h"

DEFINE_string(host, "10.192.101.15", "server host");
DEFINE_string(service, "io_uring", "target service: io_uring or blocking");
DEFINE_string(service_port_map, "io_uring:8040,blocking:8042",
              "service->port mapping, format: io_uring:8040,blocking:8042");
DEFINE_int32(threads, 16, "worker threads");
DEFINE_int32(duration_s, 60, "test duration in seconds");
DEFINE_int32(report_interval_s, 1, "report interval in seconds");
DEFINE_int64(file_size_bytes, 4LL << 30, "logical file size bound for random reads");
DEFINE_int32(len_bytes, 32 * 1024, "request len in bytes");
DEFINE_int32(timeout_ms, 5000, "rpc timeout");
DEFINE_int32(max_retry, 0, "rpc max retry");

namespace {

enum class ServiceKind { kIoUring, kBlocking };

bool ParseServiceKind(const std::string& v, ServiceKind* out) {
  if (v == "io_uring") {
    *out = ServiceKind::kIoUring;
    return true;
  }
  if (v == "blocking") {
    *out = ServiceKind::kBlocking;
    return true;
  }
  return false;
}

bool ParseServicePortMap(const std::string& text, int* io_port, int* blocking_port) {
  int parsed_io = -1;
  int parsed_blocking = -1;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    const auto pos = item.find(':');
    if (pos == std::string::npos) {
      return false;
    }
    const std::string key = item.substr(0, pos);
    const std::string val = item.substr(pos + 1);
    int port = -1;
    try {
      port = std::stoi(val);
    } catch (const std::exception&) {
      return false;
    }
    if (port <= 0 || port > 65535) {
      return false;
    }
    if (key == "io_uring") {
      parsed_io = port;
    } else if (key == "blocking") {
      parsed_blocking = port;
    } else {
      return false;
    }
  }
  if (parsed_io <= 0 || parsed_blocking <= 0) {
    return false;
  }
  *io_port = parsed_io;
  *blocking_port = parsed_blocking;
  return true;
}

struct SharedStats {
  std::mutex mu;
  std::vector<double> window_lat_ms;
  std::vector<double> all_lat_ms;
  uint64_t window_total = 0;
  uint64_t window_ok = 0;
  uint64_t window_fail = 0;
  uint64_t window_rpc_fail = 0;
  uint64_t window_app_fail = 0;
  uint64_t total = 0;
  uint64_t ok = 0;
  uint64_t fail = 0;
  uint64_t rpc_fail = 0;
  uint64_t app_fail = 0;
};

double Percentile(std::vector<double>& sorted, double q) {
  if (sorted.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (q <= 0.0) {
    return sorted.front();
  }
  if (q >= 1.0) {
    return sorted.back();
  }
  const double pos = (sorted.size() - 1) * q;
  const auto lo = static_cast<size_t>(pos);
  const auto hi = static_cast<size_t>(std::ceil(pos));
  if (lo == hi) {
    return sorted[lo];
  }
  const double frac = pos - lo;
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

void WorkerThread(const std::string& server_addr, ServiceKind kind, SharedStats* stats,
                  const std::atomic<bool>* stop) {
  brpc::Channel channel;
  brpc::ChannelOptions options;
  options.protocol = "baidu_std";
  options.timeout_ms = FLAGS_timeout_ms;
  options.max_retry = FLAGS_max_retry;
  if (channel.Init(server_addr.c_str(), nullptr, &options) != 0) {
    std::cerr << "[worker] failed to init channel to " << server_addr << '\n';
    return;
  }
  iouring_file_read::FileReadService_Stub stub(&channel);
  iouring_file_read::BlockingFileReadService_Stub blocking_stub(&channel);

  thread_local std::mt19937_64 rng(
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rng)));

  iouring_file_read::FileReadRequest req;
  while (!stop->load(std::memory_order_relaxed)) {
    const int64_t max_off = std::max<int64_t>(0, FLAGS_file_size_bytes - FLAGS_len_bytes);
    const int64_t max_slot = max_off / 4096;
    std::uniform_int_distribution<int64_t> off_dist(0, max_slot);
    const int64_t off = off_dist(rng) * 4096;
    req.set_offset(off);
    req.set_len(FLAGS_len_bytes);

    brpc::Controller cntl;
    iouring_file_read::FileReadResponse rsp;
    const auto begin = std::chrono::steady_clock::now();
    if (kind == ServiceKind::kIoUring) {
      stub.Read(&cntl, &req, &rsp, nullptr);
    } else {
      blocking_stub.Read(&cntl, &req, &rsp, nullptr);
    }
    const auto end = std::chrono::steady_clock::now();
    const double lat_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - begin)
            .count();
    const bool rpc_ok = !cntl.Failed();
    const bool app_ok = rpc_ok && rsp.code() == iouring_file_read::FILE_READ_OK;
    const bool ok = rpc_ok && app_ok;
    const bool is_rpc_fail = !rpc_ok;
    const bool is_app_fail = rpc_ok && !app_ok;

    {
      std::lock_guard<std::mutex> lk(stats->mu);
      stats->window_lat_ms.push_back(lat_ms);
      stats->all_lat_ms.push_back(lat_ms);
      ++stats->window_total;
      ++stats->total;
      if (ok) {
        ++stats->window_ok;
        ++stats->ok;
      } else {
        ++stats->window_fail;
        ++stats->fail;
        if (is_rpc_fail) {
          ++stats->window_rpc_fail;
          ++stats->rpc_fail;
        } else {
          ++stats->window_app_fail;
          ++stats->app_fail;
        }
      }
    }
  }
}

void PrintReport(double elapsed_s, uint64_t total, uint64_t ok, uint64_t fail,
                 uint64_t rpc_fail, uint64_t app_fail, std::vector<double> lats_ms,
                 const std::string& prefix, const std::string& service_name) {
  if (lats_ms.empty()) {
    std::cout << prefix << " t=" << std::fixed << std::setprecision(1) << elapsed_s
              << "s service=" << service_name << " no-samples\n";
    return;
  }
  std::sort(lats_ms.begin(), lats_ms.end());
  const double avg = std::accumulate(lats_ms.begin(), lats_ms.end(), 0.0) / lats_ms.size();
  const double p99 = Percentile(lats_ms, 0.99);
  const double p999 = Percentile(lats_ms, 0.999);
  const double p9999 = Percentile(lats_ms, 0.9999);
  const double qps = elapsed_s > 0.0 ? static_cast<double>(total) / elapsed_s : 0.0;
  const double succ = total > 0 ? (100.0 * ok / static_cast<double>(total)) : 0.0;

  std::cout << prefix << " t=" << std::fixed << std::setprecision(1) << elapsed_s << "s "
            << "service=" << service_name << " total=" << total << " ok=" << ok
            << " fail=" << fail << " (rpc=" << rpc_fail << ",app=" << app_fail << ") succ="
            << std::setprecision(2) << succ << "% qps=" << qps << " avg="
            << std::setprecision(3) << avg << "ms p99=" << p99 << "ms p999=" << p999
            << "ms p9999=" << p9999 << "ms\n";
}

}  // namespace

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_threads <= 0 || FLAGS_duration_s <= 0 || FLAGS_report_interval_s <= 0 ||
      FLAGS_len_bytes <= 0 || FLAGS_file_size_bytes < FLAGS_len_bytes) {
    std::cerr << "invalid arguments\n";
    return 1;
  }

  ServiceKind kind = ServiceKind::kIoUring;
  if (!ParseServiceKind(FLAGS_service, &kind)) {
    std::cerr << "invalid --service, expected io_uring|blocking\n";
    return 1;
  }
  int io_port = 0;
  int blocking_port = 0;
  if (!ParseServicePortMap(FLAGS_service_port_map, &io_port, &blocking_port)) {
    std::cerr << "invalid --service_port_map, expected io_uring:PORT,blocking:PORT\n";
    return 1;
  }
  const int target_port = (kind == ServiceKind::kIoUring) ? io_port : blocking_port;
  const std::string target_service_name =
      (kind == ServiceKind::kIoUring) ? "io_uring" : "blocking";
  const std::string mapped_addr = FLAGS_host + ":" + std::to_string(target_port);
  std::cout << "target=" << mapped_addr << " service=" << target_service_name
            << " map={" << FLAGS_service_port_map << "} threads=" << FLAGS_threads
            << " duration=" << FLAGS_duration_s
            << "s len=" << FLAGS_len_bytes
            << " file_size_bound=" << FLAGS_file_size_bytes << '\n';

  SharedStats stats;
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(FLAGS_threads));
  for (int i = 0; i < FLAGS_threads; ++i) {
    workers.emplace_back(WorkerThread, mapped_addr, kind, &stats, &stop);
  }

  const auto begin = std::chrono::steady_clock::now();
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(FLAGS_report_interval_s));
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(now - begin).count();

    std::vector<double> window;
    uint64_t wt = 0;
    uint64_t wok = 0;
    uint64_t wfail = 0;
    uint64_t wrpc_fail = 0;
    uint64_t wapp_fail = 0;
    {
      std::lock_guard<std::mutex> lk(stats.mu);
      window.swap(stats.window_lat_ms);
      wt = stats.window_total;
      wok = stats.window_ok;
      wfail = stats.window_fail;
      wrpc_fail = stats.window_rpc_fail;
      wapp_fail = stats.window_app_fail;
      stats.window_total = 0;
      stats.window_ok = 0;
      stats.window_fail = 0;
      stats.window_rpc_fail = 0;
      stats.window_app_fail = 0;
    }
    PrintReport(static_cast<double>(FLAGS_report_interval_s), wt, wok, wfail, wrpc_fail,
                wapp_fail, window, "[window]", target_service_name);

    if (elapsed_s >= FLAGS_duration_s) {
      break;
    }
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& t : workers) {
    t.join();
  }

  const auto end = std::chrono::steady_clock::now();
  const double total_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();

  std::vector<double> all;
  uint64_t total = 0;
  uint64_t ok = 0;
  uint64_t fail = 0;
  uint64_t rpc_fail = 0;
  uint64_t app_fail = 0;
  {
    std::lock_guard<std::mutex> lk(stats.mu);
    all = stats.all_lat_ms;
    total = stats.total;
    ok = stats.ok;
    fail = stats.fail;
    rpc_fail = stats.rpc_fail;
    app_fail = stats.app_fail;
  }
  PrintReport(total_s, total, ok, fail, rpc_fail, app_fail, std::move(all), "[final ]",
              target_service_name);
  return 0;
}
