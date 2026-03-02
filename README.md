# brpc-iouring

Standalone brpc + io_uring file-read server.

## Responsibilities

- `bootstrap_server_deps.sh` only prepares dependencies and `brpc`.
- Service build is done by CMake commands in this README.

Dependency outputs:

- `third_party/src`
- `third_party/install`

Service binary output (after CMake build):

- `build/file_read_server`

## 1) Bootstrap dependencies

```bash
cd ./brpc-iouring
./bootstrap_server_deps.sh
```

The script:

- installs required system packages
- builds and installs `zlib`, `openssl`, `gflags`, `protobuf`, `snappy`, `leveldb`, `liburing`
- clones and builds `brpc` from `malikhou/add_active_task`

It does **not** build this repository's server binary.

## 2) Build server

```bash
cd ./brpc-iouring
rm -rf build
cmake -S . -B build \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DTHIRD_PARTY_ROOT="$PWD/third_party/install"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
ln -sfn build/compile_commands.json compile_commands.json
```

## 3) Start server

Create an aligned test file for O_DIRECT:

```bash
dd if=/dev/zero of=/tmp/iouring-directio.bin bs=4096 count=256
```

Run:

```bash
./build/file_read_server \
  --port=8040 \
  --monitor_port=8041 \
  --read_tag=1 \
  --monitor_tag=0 \
  --read_num_threads=12 \
  --monitor_num_threads=4 \
  --file_path=/tmp/iouring-directio.bin
```

## 4) Verify

```bash
curl -s http://127.0.0.1:8041/status | sed -n '1,5p'
```

```bash
ldd ./build/file_read_server | sed -n '/libbrpc\|liburing\|libprotobuf\|libgflags\|libleveldb\|libsnappy\|libssl\|libcrypto\|libz/p'
```

## Clean rebuild

```bash
rm -rf build third_party
./bootstrap_server_deps.sh
```

## Runtime notes

- Linux kernel must support io_uring.
- Runtime policy must allow io_uring syscalls.
- `--file_path` target must exist for O_DIRECT open.

If startup log contains:

```text
worker io_uring init failed, rc=1
```

this is usually an environment/kernel restriction, not a dependency link failure.

## Docker

```bash
docker build -t brpc-iouring:latest .
```

```bash
docker run --rm -it \
  --privileged \
  --security-opt seccomp=unconfined \
  -p 8040:8040 -p 8041:8041 \
  brpc-iouring:latest
```

## Optional bootstrap overrides

```bash
BRPC_BRANCH=malikhou/add_active_task \
BRPC_REPO=https://github.com/MalikHou/brpc.git \
./bootstrap_server_deps.sh
```
