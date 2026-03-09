# brpc iouring vs pread benchmark

一个干净的 `server + client` 对比工程：
- `io_uring` 路径：基于 active task，在 `Harvest` 里做提交、收割、唤醒。
- `pread` 路径：直接同步 `pread`。

## 安装依赖

```bash
./bootstrap_server_deps.sh
```

## 1. 清空并更新本地 third_party 依赖

```bash
./bootstrap_server_deps.sh --clean
```

如果希望 brpc 仓库也完全重拉（而不是复用已有 `third_party/src/brpc`），可用：

```bash
./bootstrap_server_deps.sh --clean --reclone-brpc
```

该脚本会强制对齐 `origin/malikhou/add_active_task` 最新提交，依赖与产物均位于本地 `third_party/` 目录。

## 2. 编译（gcc + clang 都跑一次，目录分离）

```bash
./scripts/build_gcc_clang.sh
```

`FORCE_STATIC_THIRD_PARTY=ON` 会优先链接 `third_party` 下的静态库（`.a`）。

等价的手工 CMake 命令（目录按 `gcc_*` / `clang_*` 区分）：

```bash
cmake -S server -B gcc_server \
  -DCMAKE_CXX_COMPILER=g++ \
  -DPROTOC_EXECUTABLE="$PWD/third_party/install/bin/protoc" \
  -DFORCE_STATIC_THIRD_PARTY=ON \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DDEPS_ROOT="$PWD/third_party/install"
cmake --build gcc_server -j"$(nproc)"

cmake -S client -B gcc_client \
  -DCMAKE_CXX_COMPILER=g++ \
  -DPROTOC_EXECUTABLE="$PWD/third_party/install/bin/protoc" \
  -DFORCE_STATIC_THIRD_PARTY=ON \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DDEPS_ROOT="$PWD/third_party/install"
cmake --build gcc_client -j"$(nproc)"

cmake -S server -B clang_server \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DPROTOC_EXECUTABLE="$PWD/third_party/install/bin/protoc" \
  -DFORCE_STATIC_THIRD_PARTY=ON \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DDEPS_ROOT="$PWD/third_party/install"
cmake --build clang_server -j"$(nproc)"

cmake -S client -B clang_client \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DPROTOC_EXECUTABLE="$PWD/third_party/install/bin/protoc" \
  -DFORCE_STATIC_THIRD_PARTY=ON \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DDEPS_ROOT="$PWD/third_party/install"
cmake --build clang_client -j"$(nproc)"
```

产物：
- `gcc_server/read_bench_server`
- `gcc_client/read_bench_client`
- `clang_server/read_bench_server`
- `clang_client/read_bench_client`

## 3. 准备测试文件

```bash
dd if=/dev/urandom of=/data/read_bench.data bs=4M count=1024 status=progress
```

## 4. 启动 server（单进程 3 个 server / 3 个 tag）

```bash
# 或 ./clang_server/read_bench_server
./gcc_server/read_bench_server \
  --monitor_port=8010 \
  --monitor_tag=0 \
  --port=8002 \
  --iouring_tag=1 \
  --pread_port=8003 \
  --pread_tag=2 \
  --file_path=/data/read_bench.data \
  --num_threads=5 \
  --monitor_num_threads=5 \
  --iouring_queue_depth=256 \
  --max_read_size=65536 \
  --file_direct_io=true
```

说明：
- `monitor` server 只提供 builtin 服务，绑定 `tag0`
- `io_uring` server 只提供 `IoUringReadService`，绑定 `tag1`
- `pread` server 只提供 `PreadReadService`，绑定 `tag2`
- 服务端和客户端都会在启动时强制打开 `event_dispatcher_edisp_unsched=true`
- `io_uring` 读等待使用无超时死等（请求不返回就持续等待）
- `--file_open_flags` 支持直接传 `open(2)` flags 数值（默认 `O_RDONLY|O_CLOEXEC`）
- 需要 direct io 时，优先使用 `--file_direct_io=true`（会自动追加 `O_DIRECT`）
- 开启 `--file_direct_io=true` 时，请求 `offset/len` 需要按启动日志里的 `direct_io_offset_align` 对齐
- direct io 场景建议在 client 侧设置 `--request_align=<direct_io_offset_align>`，并保证 `--read_size` 是该值的整数倍
- 若 `--max_read_size < direct_io_offset_align`，server 会在启动时直接报错退出

## 5. 启动 client（可按参数选择 io_uring / pread）

```bash
# 或 ./clang_client/read_bench_client
./gcc_client/read_bench_client \
  --service=io_uring \
  --iouring_server_addr=127.0.0.1:8002 \
  --pread_server_addr=127.0.0.1:8003 \
  --concurrency=8 \
  --duration_s=30 \
  --request_align=4096 \
  --read_size=4096 \
  --file_size=$((64*1024*1024))
```

可选值：
- `--service=io_uring`：只请求 `IoUringReadService`
- `--service=pread`：只请求 `PreadReadService`

示例输出：

```text
no-timeout mode: channel timeout_ms=-1, connect_timeout_ms=-1
timed benchmark: duration_s=30, per-second metrics enabled
[io_uring] sec=1 ok=73412 fail=0 qps=73412.00 mbps=286.77 avg_us=201 p50_us=188 p99_us=451 p999_us=690 p9999_us=1210
...
[pread] sec=1 ok=91420 fail=0 qps=91420.00 mbps=357.11 avg_us=163 p50_us=158 p99_us=280 p999_us=410 p9999_us=740
=== final ===
io_uring   total_ok=2190460 total_fail=0 qps=73015.33 mbps=285.21 avg_us=198 p50_us=186 p99_us=439 p999_us=675 p9999_us=1188
pread      total_ok=2740832 total_fail=0 qps=91361.07 mbps=356.88 avg_us=160 p50_us=156 p99_us=276 p999_us=404 p9999_us=731
speedup_qps(io_uring/pread)=0.799
speedup_p99_latency(pread/io_uring)=0.629
```

## 6. perf 采样脚本

按进程 PID 采样（`perf stat + pidstat`）：

```bash
./scripts/collect_perf_by_pid.sh --pid <PID> --duration_s 30
```

## 7. 代码结构

- `file_read.proto`: 两个服务 `IoUringReadService` / `PreadReadService`
- `server/main.cpp`: 单进程三 server（monitor/iouring/pread）
- `client/main.cpp`: 并发压测并输出 QPS、吞吐、延迟分位
- `server/CMakeLists.txt`、`client/CMakeLists.txt`: 分离构建配置
