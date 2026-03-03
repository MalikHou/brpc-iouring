# brpc-iouring

`brpc + io_uring` 文件读取服务（固定 `O_DIRECT` 路径）。

服务提供两个 RPC 接口：
- `io_uring` 服务（端口默认 `8040`）
- `blocking` 服务（端口默认 `8042`）

## 目录与产物

- 依赖安装目录：`third_party/install`
- brpc 源码与构建目录：`third_party/src/brpc`
- 服务端二进制：`build/file_read_server`
- 压测客户端二进制：`scripts/brpc_latency_monitor`

## 1. 依赖安装

在仓库根目录执行：

```bash
cd ./brpc-iouring
./bootstrap_server_deps.sh
```

该脚本会：
- 安装系统依赖（`build-essential`、`cmake`、`git`、`curl`、`pkg-config` 等）
- 编译并安装三方库：`zlib`、`openssl`、`gflags`、`protobuf`、`snappy`、`leveldb`、`liburing`
- 拉取并编译指定分支的 `brpc`

可选：指定 brpc 仓库或分支

```bash
BRPC_REPO=https://github.com/MalikHou/brpc.git \
BRPC_BRANCH=malikhou/add_active_task \
./bootstrap_server_deps.sh
```

## 2. 编译服务端

```bash
cd ./brpc-iouring
rm -rf build
cmake -S . -B build \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DTHIRD_PARTY_ROOT="$PWD/third_party/install"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
ln -sfn build/compile_commands.json compile_commands.json
```

## 3. 准备测试文件（O_DIRECT）

推荐生成 4GiB 文件：

```bash
dd if=/dev/urandom of=/tmp/iouring-directio.bin bs=4M count=1024 status=progress
```

（或更快的零填充版本）

```bash
dd if=/dev/zero of=/tmp/iouring-directio.bin bs=4M count=1024
```

## 4. 启动服务端

```bash
./build/file_read_server \
  --port=8040 \
  --blocking_port=8042 \
  --monitor_port=8041 \
  --read_tag=1 \
  --blocking_tag=2 \
  --monitor_tag=0 \
  --read_num_threads=12 \
  --blocking_num_threads=12 \
  --monitor_num_threads=4 \
  --file_path=/tmp/iouring-directio.bin
```

说明：
- 当前版本固定 direct-io 路径；请求大小由客户端 `req.len` 控制。
- `req.len` 仅支持这五档：`1024`、`4096`、`16384`、`32768`、`65536`。
- 兼容保留参数（如 `--iouring_mode`、`--read_len_bytes`）会被忽略。

## 5. 启动后检查

```bash
curl -s http://127.0.0.1:8041/status | sed -n '1,20p'
curl -s http://127.0.0.1:8041/vars | egrep 'file_read_iops|file_read_blocking_iops'
```

## 6. 编译客户端

```bash
c++ -O2 -std=c++17 scripts/brpc_latency_monitor.cpp build/file_read.pb.cc \
  -Ibuild -Ithird_party/install/include -Ithird_party/src/brpc/output/include \
  -Lthird_party/src/brpc/output/lib -Lthird_party/install/lib \
  -Wl,-rpath,'$ORIGIN/../third_party/src/brpc/output/lib:$ORIGIN/../third_party/install/lib' \
  -lbrpc -lprotobuf -lgflags -lleveldb -lsnappy -lssl -lcrypto -lz -luring -ldl -lrt -pthread \
  -o scripts/brpc_latency_monitor
```

## 7. 客户端使用方法

### 7.1 io_uring 服务

```bash
./scripts/brpc_latency_monitor \
  --host=127.0.0.1 \
  --service=io_uring \
  --service_port_map=io_uring:8040,blocking:8042 \
  --threads=32 \
  --duration_s=30 \
  --timeout_ms=10000 \
  --len_bytes=32768 \
  --file_size_bytes=$((4*1024*1024*1024))
```

### 7.2 blocking 服务

```bash
./scripts/brpc_latency_monitor \
  --host=127.0.0.1 \
  --service=blocking \
  --service_port_map=io_uring:8040,blocking:8042 \
  --threads=32 \
  --duration_s=30 \
  --timeout_ms=10000 \
  --len_bytes=32768 \
  --file_size_bytes=$((4*1024*1024*1024))
```

### 7.3 批量测试五档读大小

```bash
for len in 1024 4096 16384 32768 65536; do
  ./scripts/brpc_latency_monitor \
    --host=127.0.0.1 \
    --service=io_uring \
    --service_port_map=io_uring:8040,blocking:8042 \
    --threads=32 \
    --duration_s=20 \
    --timeout_ms=10000 \
    --len_bytes=${len} \
    --file_size_bytes=$((4*1024*1024*1024))
done
```

### 7.4 客户端输出指标

窗口与最终输出都会包含：
- `iface_qps`（客户端接口 QPS）
- `avg`（平均延迟）
- `p99` / `p999` / `p9999`（延迟分位）

## 8. 清理重建

```bash
rm -rf build third_party
./bootstrap_server_deps.sh
```
