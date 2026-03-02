# Deployment Bootstrap

Server is now a root-level project.
Source-built dependencies are under:

- `third_party/src`
- `third_party/install`

## 1) Bootstrap and build server

```bash
cd ./brpc-iouring
bash ./bootstrap_server_deps.sh
```

## 2) Start server

Prepare a file for O_DIRECT reads first:

```bash
dd if=/dev/zero of=/tmp/iouring-directio.bin bs=4096 count=256
```

Then start server:

```bash
./build/file_read_server \
  --port=8040 \
  --monitor_port=8041 \
  --read_tag=1 \
  --monitor_tag=0 \
  --read_num_threads=12 \
  --monitor_num_threads=5 \
  --task_group_ntags=2 \
  --file_path=/tmp/iouring-directio.bin
```

## Runtime prerequisites

- Linux kernel with io_uring support.
- Process must be allowed to use io_uring syscalls (container seccomp/profile may block).
- `file_path` must exist for O_DIRECT open.

If startup logs show `worker io_uring init failed, rc=1`, check kernel and runtime
restrictions first (for containers, use privileged mode and unconfined seccomp).
