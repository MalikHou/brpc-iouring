# Deployment Bootstrap

Server is now a root-level project.
Source-built dependencies are under:

- `third_party/src`
- `third_party/install`

## 1) Bootstrap and build server

```bash
cd /root/brpc-iouring
bash ./bootstrap_server_deps.sh
```

## 2) Start server

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
