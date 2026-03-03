#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
DEFINE_int32(len_bytes, 32 * 1024,
             "request len in bytes, must be one of 1024/4096/16384/32768/65536");
DEFINE_int32(timeout_ms, 5000, "rpc timeout");
DEFINE_int32(max_retry, 0, "rpc max retry");
DEFINE_int32(client_graceful_drain_ms, 5000,
             "after duration_s, stop issuing new RPCs and drain inflight requests");
DEFINE_string(connection_type, "",
              "brpc ChannelOptions.connection_type, empty means brpc default");
DEFINE_int32(rpc_fail_reason_top_n, 3,
             "print top N rpc fail reasons in final report, <=0 disables output");
DEFINE_bool(set_log_id, true,
            "set brpc Controller.log_id on each request for end-to-end correlation");
DEFINE_bool(set_request_id, true,
            "set brpc Controller.request_id using the generated log_id");
DEFINE_uint64(log_id_seed, 0,
              "starting log_id, 0 means auto-seed from steady clock");
DEFINE_int32(fail_sample_limit, 20,
             "keep first N failure samples and print them in final report");
DEFINE_bool(print_fail_sample_realtime, false,
            "print one-line failure samples immediately when they happen");

namespace {

enum class ServiceKind { kIoUring, kBlocking };

std::atomic<uint64_t> g_next_log_id{1};

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

bool IsSupportedLenBytes(int len) {
  return len == 1024 || len == 4096 || len == 16 * 1024 || len == 32 * 1024 ||
         len == 64 * 1024;
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
  std::unordered_map<int, uint64_t> rpc_fail_reason_counts;
  std::unordered_map<int, std::string> rpc_fail_reason_samples;
  std::unordered_map<int, uint64_t> app_fail_reason_counts;
  std::unordered_map<int, std::string> app_fail_reason_samples;
  std::vector<std::string> fail_samples;
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

std::string SanitizeInlineText(const std::string& text) {
  if (text.empty()) {
    return "";
  }
  std::string out;
  out.reserve(text.size());
  for (unsigned char ch : text) {
    if (std::isspace(ch)) {
      out.push_back('_');
    } else if (ch == '|' || ch == ',') {
      out.push_back(';');
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  constexpr size_t kMaxLen = 120;
  if (out.size() > kMaxLen) {
    out.resize(kMaxLen - 3);
    out.append("...");
  }
  return out;
}

std::string FormatRpcFailReasons(
    const std::unordered_map<int, uint64_t>& counts,
    const std::unordered_map<int, std::string>& samples, int top_n) {
  if (counts.empty() || top_n <= 0) {
    return "none";
  }
  std::vector<std::pair<int, uint64_t>> sorted;
  sorted.reserve(counts.size());
  for (const auto& kv : counts) {
    sorted.push_back(kv);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<int, uint64_t>& a, const std::pair<int, uint64_t>& b) {
              if (a.second != b.second) {
                return a.second > b.second;
              }
              return a.first < b.first;
            });
  const size_t limit =
      std::min(sorted.size(), static_cast<size_t>(std::max(0, top_n)));
  if (limit == 0) {
    return "none";
  }
  std::ostringstream os;
  for (size_t i = 0; i < limit; ++i) {
    if (i > 0) {
      os << "|";
    }
    const int code = sorted[i].first;
    os << code << ":" << sorted[i].second;
    const auto it = samples.find(code);
    if (it != samples.end() && !it->second.empty()) {
      os << ":" << it->second;
    }
  }
  return os.str();
}

uint64_t ResolveInitialLogIdSeed() {
  if (FLAGS_log_id_seed != 0) {
    return FLAGS_log_id_seed;
  }
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const uint64_t seed = (now_ns & 0x7fffffffffffffffull) | 1ull;
  return seed == 0 ? 1 : seed;
}

std::string FormatFailSample(bool rpc_fail, uint64_t log_id, int64_t off, double lat_ms,
                             int rpc_error_code, const std::string& rpc_error_text,
                             int app_code, const std::string& app_msg) {
  std::ostringstream os;
  if (rpc_fail) {
    os << "type=rpc"
       << " log_id=" << log_id
       << " off=" << off
       << " lat_ms=" << std::fixed << std::setprecision(3) << lat_ms
       << " rpc_code=" << rpc_error_code
       << " rpc_text=" << (rpc_error_text.empty() ? "-" : rpc_error_text);
    return os.str();
  }
  os << "type=app"
     << " log_id=" << log_id
     << " off=" << off
     << " lat_ms=" << std::fixed << std::setprecision(3) << lat_ms
     << " app_code=" << app_code
     << " app_msg=" << (app_msg.empty() ? "-" : app_msg);
  return os.str();
}

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
                  const std::atomic<bool>* stop_issue_new, std::atomic<int>* live_workers) {
  struct ExitGuard {
    std::atomic<int>* counter = nullptr;
    ~ExitGuard() {
      if (counter != nullptr) {
        counter->fetch_sub(1, std::memory_order_relaxed);
      }
    }
  } exit_guard{live_workers};

  brpc::Channel channel;
  brpc::ChannelOptions options;
  options.protocol = "baidu_std";
  options.timeout_ms = FLAGS_timeout_ms;
  options.max_retry = FLAGS_max_retry;
  if (!FLAGS_connection_type.empty()) {
    options.connection_type = FLAGS_connection_type;
  }
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
  while (true) {
    if (stop_issue_new->load(std::memory_order_relaxed)) {
      break;
    }
    const int64_t max_off = std::max<int64_t>(0, FLAGS_file_size_bytes - FLAGS_len_bytes);
    const int64_t max_slot = max_off / 4096;
    std::uniform_int_distribution<int64_t> off_dist(0, max_slot);
    const int64_t off = off_dist(rng) * 4096;
    req.set_offset(off);
    req.set_len(FLAGS_len_bytes);

    brpc::Controller cntl;
    uint64_t log_id = 0;
    if (FLAGS_set_log_id) {
      log_id = g_next_log_id.fetch_add(1, std::memory_order_relaxed);
      cntl.set_log_id(log_id);
      if (FLAGS_set_request_id) {
        cntl.set_request_id("bench-" + std::to_string(log_id));
      }
    }
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
    int rpc_error_code = 0;
    std::string rpc_error_text;
    int app_error_code = 0;
    std::string app_error_text;
    if (is_rpc_fail) {
      rpc_error_code = cntl.ErrorCode();
      rpc_error_text = SanitizeInlineText(cntl.ErrorText());
    } else if (is_app_fail) {
      app_error_code = rsp.code();
      app_error_text = SanitizeInlineText(rsp.message());
    }
    const std::string fail_sample =
        ok ? std::string() : FormatFailSample(is_rpc_fail, log_id, off, lat_ms,
                                              rpc_error_code, rpc_error_text,
                                              app_error_code, app_error_text);
    if (!ok && FLAGS_print_fail_sample_realtime) {
      std::cerr << "[fail ] " << fail_sample << '\n';
    }

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
          ++stats->rpc_fail_reason_counts[rpc_error_code];
          if (!rpc_error_text.empty()) {
            const auto inserted =
                stats->rpc_fail_reason_samples.emplace(rpc_error_code, rpc_error_text);
            if (!inserted.second && inserted.first->second.empty()) {
              inserted.first->second = rpc_error_text;
            }
          }
        } else {
          ++stats->window_app_fail;
          ++stats->app_fail;
          ++stats->app_fail_reason_counts[app_error_code];
          if (!app_error_text.empty()) {
            const auto inserted =
                stats->app_fail_reason_samples.emplace(app_error_code, app_error_text);
            if (!inserted.second && inserted.first->second.empty()) {
              inserted.first->second = app_error_text;
            }
          }
        }
        if (FLAGS_fail_sample_limit > 0 &&
            stats->fail_samples.size() < static_cast<size_t>(FLAGS_fail_sample_limit)) {
          stats->fail_samples.push_back(fail_sample);
        }
      }
    }
  }
}

void PrintReport(double elapsed_s, uint64_t total, uint64_t ok, uint64_t fail,
                 uint64_t rpc_fail, uint64_t app_fail, std::vector<double> lats_ms,
                 const std::string& prefix, const std::string& service_name, double issue_s = -1.0,
                 double tail_s = -1.0, uint64_t tail_total = 0, uint64_t tail_fail = 0,
                 uint64_t tail_rpc_fail = 0, uint64_t tail_app_fail = 0,
                 const std::string& rpc_fail_reasons = "",
                 const std::string& app_fail_reasons = "") {
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
            << std::setprecision(2) << succ << "% iface_qps=" << qps << " avg="
            << std::setprecision(3) << avg << "ms p99=" << p99 << "ms p999=" << p999
            << "ms p9999=" << p9999 << "ms";
  if (issue_s >= 0.0 && tail_s >= 0.0) {
    std::cout << " issue_t=" << std::setprecision(1) << issue_s
              << "s tail_t=" << std::setprecision(1) << tail_s
              << "s wall_t=" << std::setprecision(1) << elapsed_s
              << "s tail_total=" << tail_total
              << " tail_fail=" << tail_fail
              << " tail_rpc_fail=" << tail_rpc_fail
              << " tail_app_fail=" << tail_app_fail
              << " rpc_fail_reasons="
              << (rpc_fail_reasons.empty() ? "none" : rpc_fail_reasons)
              << " app_fail_reasons="
              << (app_fail_reasons.empty() ? "none" : app_fail_reasons);
  }
  std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_threads <= 0 || FLAGS_duration_s <= 0 || FLAGS_report_interval_s <= 0 ||
      FLAGS_len_bytes <= 0 || FLAGS_file_size_bytes < FLAGS_len_bytes ||
      FLAGS_client_graceful_drain_ms < 0 || FLAGS_rpc_fail_reason_top_n < 0 ||
      FLAGS_fail_sample_limit < 0) {
    std::cerr << "invalid arguments\n";
    return 1;
  }
  if (!IsSupportedLenBytes(FLAGS_len_bytes)) {
    std::cerr << "invalid --len_bytes, must be one of 1024/4096/16384/32768/65536\n";
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
  const uint64_t log_id_seed = ResolveInitialLogIdSeed();
  g_next_log_id.store(log_id_seed, std::memory_order_relaxed);
  std::cout << "target=" << mapped_addr << " service=" << target_service_name
            << " map={" << FLAGS_service_port_map << "} threads=" << FLAGS_threads
            << " duration=" << FLAGS_duration_s
            << "s len=" << FLAGS_len_bytes
            << " file_size_bound=" << FLAGS_file_size_bytes
            << " drain_ms=" << FLAGS_client_graceful_drain_ms
            << " set_log_id=" << (FLAGS_set_log_id ? "1" : "0")
            << " set_request_id=" << (FLAGS_set_request_id ? "1" : "0")
            << " log_id_seed=" << log_id_seed
            << " fail_sample_limit=" << FLAGS_fail_sample_limit << '\n';

  SharedStats stats;
  std::atomic<bool> stop_issue_new{false};
  std::atomic<int> live_workers{FLAGS_threads};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(FLAGS_threads));
  for (int i = 0; i < FLAGS_threads; ++i) {
    workers.emplace_back(WorkerThread, mapped_addr, kind, &stats, &stop_issue_new,
                         &live_workers);
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

  stop_issue_new.store(true, std::memory_order_relaxed);
  const auto issue_end = std::chrono::steady_clock::now();
  uint64_t issue_snapshot_total = 0;
  uint64_t issue_snapshot_fail = 0;
  uint64_t issue_snapshot_rpc_fail = 0;
  uint64_t issue_snapshot_app_fail = 0;
  {
    std::lock_guard<std::mutex> lk(stats.mu);
    issue_snapshot_total = stats.total;
    issue_snapshot_fail = stats.fail;
    issue_snapshot_rpc_fail = stats.rpc_fail;
    issue_snapshot_app_fail = stats.app_fail;
  }
  const auto drain_begin = std::chrono::steady_clock::now();
  const auto drain_deadline =
      drain_begin + std::chrono::milliseconds(FLAGS_client_graceful_drain_ms);
  while (live_workers.load(std::memory_order_relaxed) > 0 &&
         std::chrono::steady_clock::now() < drain_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (live_workers.load(std::memory_order_relaxed) > 0) {
    std::cerr << "[drain] deadline reached with " << live_workers.load(std::memory_order_relaxed)
              << " worker(s) still waiting for inflight RPCs\n";
  }
  for (auto& t : workers) {
    t.join();
  }

  const auto end = std::chrono::steady_clock::now();
  const double wall_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
  const double issue_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(issue_end - begin).count();
  const double tail_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - issue_end).count();

  std::vector<double> all;
  std::unordered_map<int, uint64_t> rpc_fail_reason_counts;
  std::unordered_map<int, std::string> rpc_fail_reason_samples;
  std::unordered_map<int, uint64_t> app_fail_reason_counts;
  std::unordered_map<int, std::string> app_fail_reason_samples;
  std::vector<std::string> fail_samples;
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
    rpc_fail_reason_counts = stats.rpc_fail_reason_counts;
    rpc_fail_reason_samples = stats.rpc_fail_reason_samples;
    app_fail_reason_counts = stats.app_fail_reason_counts;
    app_fail_reason_samples = stats.app_fail_reason_samples;
    fail_samples = stats.fail_samples;
  }
  const auto sub_or_zero = [](uint64_t after, uint64_t before) -> uint64_t {
    return after >= before ? (after - before) : 0;
  };
  const uint64_t tail_total = sub_or_zero(total, issue_snapshot_total);
  const uint64_t tail_fail = sub_or_zero(fail, issue_snapshot_fail);
  const uint64_t tail_rpc_fail = sub_or_zero(rpc_fail, issue_snapshot_rpc_fail);
  const uint64_t tail_app_fail = sub_or_zero(app_fail, issue_snapshot_app_fail);
  const std::string rpc_fail_reasons = FormatRpcFailReasons(
      rpc_fail_reason_counts, rpc_fail_reason_samples, FLAGS_rpc_fail_reason_top_n);
  const std::string app_fail_reasons = FormatRpcFailReasons(
      app_fail_reason_counts, app_fail_reason_samples, FLAGS_rpc_fail_reason_top_n);
  PrintReport(wall_s, total, ok, fail, rpc_fail, app_fail, std::move(all), "[final ]",
              target_service_name, issue_s, tail_s, tail_total, tail_fail, tail_rpc_fail,
              tail_app_fail, rpc_fail_reasons, app_fail_reasons);
  if (!fail_samples.empty()) {
    for (size_t i = 0; i < fail_samples.size(); ++i) {
      std::cout << "[final-fail] idx=" << (i + 1) << " " << fail_samples[i] << '\n';
    }
  }
  return 0;
}
