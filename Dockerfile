FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive
WORKDIR /workspace

# Base tools. bootstrap_server_deps.sh will still install required packages,
# this keeps container behavior predictable.
RUN apt-get update && apt-get install -y \
    ca-certificates \
    curl \
    git \
    cmake \
    make \
    perl \
    python3 \
    pkg-config \
    build-essential \
    linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /workspace

# Full dependency bootstrap + server build.
RUN ./bootstrap_server_deps.sh

EXPOSE 8040 8041

CMD ["./build/file_read_server", "--port=8040", "--monitor_port=8041", "--read_tag=1", "--monitor_tag=0", "--read_num_threads=12", "--monitor_num_threads=4", "--file_path=/tmp/iouring-directio.bin"]
