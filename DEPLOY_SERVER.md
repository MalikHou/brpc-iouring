# Standalone Server Deployment

Root directory supports building the file-read server as an independent CMake project.
Source-built dependencies are kept under:

- `third_party/src` (source trees)
- `third_party/install` (installed headers/libs)

## One-shot bootstrap

```bash
cd /root/brpc-iouring
sh ./bootstrap_server_deps.sh
```

## Recommended build commands (inside project root)

Use this sequence to avoid stale CMake cache/include path issues:

```bash
cd ./brpc-iouring
rm -rf build
cmake -S . -B build \
  -DBRPC_ROOT="$PWD/third_party/src/brpc/output" \
  -DTHIRD_PARTY_ROOT="$PWD/third_party/install"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
ln -sfn build/compile_commands.json compile_commands.json
```

Optional environment overrides:

```bash
BRPC_BRANCH=malikhou/add_active_task \
BRPC_REPO=https://github.com/MalikHou/brpc.git \
sh ./bootstrap_server_deps.sh
```

The script builds all required third-party dependencies from source into
`third_party/install` (`zlib`, `openssl`, `gflags`, `protobuf+protoc`,
`snappy`, `leveldb`, `liburing`) and builds `brpc` under
`third_party/src/brpc/output`.

No `/usr/include` or `/usr/lib*` paths are passed to `config_brpc.sh` or
standalone server CMake dependency lookup.

## Clean rebuild

```bash
rm -rf build third_party
sh ./bootstrap_server_deps.sh
```

## Start standalone server

Create an aligned test file for O_DIRECT:

```bash
dd if=/dev/zero of=/tmp/iouring-directio.bin bs=4096 count=256
```

Then start:

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

## Verify

```bash
curl -s http://127.0.0.1:8041/status | sed -n '1,5p'
```

## Isolation verification

```bash
ldd ./build/file_read_server | sed -n '/libbrpc\|liburing\|libprotobuf\|libgflags\|libleveldb\|libsnappy\|libssl\|libcrypto\|libz/p'
```

You should see third-party libs resolved from `third_party/install/lib`
for the critical dependency set above.

## Runtime requirements and troubleshooting

- Kernel must support io_uring.
- Runtime policy must allow io_uring syscalls.
- If startup log contains `worker io_uring init failed, rc=1`, it is usually an
  environment/kernel restriction, not a dependency link failure.

For container runtime testing, prefer:

```bash
docker run --rm -it \
  --privileged \
  --security-opt seccomp=unconfined \
  -p 8040:8040 -p 8041:8041 \
  brpc-iouring:latest
```
