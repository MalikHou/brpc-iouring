#!/bin/sh
set -eu

# Bootstrap dependencies for standalone server build.
# This script is intended to be run from any location.
# It installs system packages, builds liburing, builds brpc active-task branch,
# then builds this server with the root CMakeLists.txt.

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}"
THIRD_PARTY_DIR="${SCRIPT_DIR}/third_party"
THIRD_PARTY_SRC_DIR="${THIRD_PARTY_DIR}/src"
THIRD_PARTY_INSTALL_DIR="${THIRD_PARTY_DIR}/install"
BRPC_DIR="${THIRD_PARTY_SRC_DIR}/brpc"
LIBURING_DIR="${THIRD_PARTY_SRC_DIR}/liburing"
ZLIB_DIR="${THIRD_PARTY_SRC_DIR}/zlib"
OPENSSL_DIR="${THIRD_PARTY_SRC_DIR}/openssl"
GFLAGS_DIR="${THIRD_PARTY_SRC_DIR}/gflags"
PROTOBUF_DIR="${THIRD_PARTY_SRC_DIR}/protobuf"
SNAPPY_DIR="${THIRD_PARTY_SRC_DIR}/snappy"
LEVELDB_DIR="${THIRD_PARTY_SRC_DIR}/leveldb"
SERVER_BUILD_DIR="${SCRIPT_DIR}/build"
BRPC_BRANCH="${BRPC_BRANCH:-malikhou/add_active_task}"
BRPC_REPO="${BRPC_REPO:-https://github.com/MalikHou/brpc.git}"
ZLIB_VERSION="${ZLIB_VERSION:-1.3.1}"
OPENSSL_VERSION="${OPENSSL_VERSION:-openssl-1.1.1w}"
GFLAGS_VERSION="${GFLAGS_VERSION:-v2.2.2}"
PROTOBUF_VERSION="${PROTOBUF_VERSION:-v3.19.6}"
SNAPPY_VERSION="${SNAPPY_VERSION:-1.1.10}"
LEVELDB_VERSION="${LEVELDB_VERSION:-1.23}"

if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    echo "Need root privileges for apt/install. Run as root or install sudo." >&2
    exit 1
  fi
else
  SUDO=""
fi

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
BUILD_DIR="${THIRD_PARTY_DIR}/build"
LOCAL_BIN="${THIRD_PARTY_INSTALL_DIR}/bin"
LOCAL_INC="${THIRD_PARTY_INSTALL_DIR}/include"
LOCAL_LIB="${THIRD_PARTY_INSTALL_DIR}/lib"
LOCAL_PKGCFG="${LOCAL_LIB}/pkgconfig"

apt_update_or_die() {
  APT_LOG="$(mktemp)"
  if ${SUDO} apt-get update >"${APT_LOG}" 2>&1; then
    cat "${APT_LOG}"
    rm -f "${APT_LOG}"
    return 0
  fi

  cat "${APT_LOG}" >&2
  rm -f "${APT_LOG}"
  echo "[error] apt-get update failed." >&2
  echo "[hint] Check invalid/obsolete apt sources (common: apt.llvm.org for unsupported distro codename)." >&2
  for SRC_FILE in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
    [ -f "${SRC_FILE}" ] || continue
    if grep -n "apt\\.llvm\\.org" "${SRC_FILE}" >/dev/null 2>&1; then
      echo "[hint] llvm source in ${SRC_FILE}:" >&2
      grep -n "apt\\.llvm\\.org" "${SRC_FILE}" >&2 || true
    fi
  done
  echo "[hint] Comment out invalid source lines, run 'apt-get update', then rerun this script." >&2
  exit 1
}

ensure_dir() {
  mkdir -p "$1"
}

download_tarball() {
  URL="$1"
  OUT="$2"
  if [ ! -f "${OUT}" ]; then
    curl -fsSL "${URL}" -o "${OUT}"
  fi
}

extract_strip1() {
  TAR_FILE="$1"
  DST_DIR="$2"
  rm -rf "${DST_DIR}"
  mkdir -p "${DST_DIR}"
  tar -xf "${TAR_FILE}" -C "${DST_DIR}" --strip-components=1
}

clone_or_update() {
  REPO_URL="$1"
  DST_DIR="$2"
  REF="$3"
  if [ ! -d "${DST_DIR}/.git" ]; then
    git clone --branch "${REF}" --depth 1 "${REPO_URL}" "${DST_DIR}"
  else
    (
      cd "${DST_DIR}"
      git fetch --depth 1 origin "${REF}"
      git checkout "${REF}"
      git reset --hard FETCH_HEAD
    )
  fi
}

check_local_dep() {
  HEADER_PATH="$1"
  LIB_PATH="$2"
  if [ ! -f "${LOCAL_INC}/${HEADER_PATH}" ]; then
    echo "[error] missing header: ${LOCAL_INC}/${HEADER_PATH}" >&2
    exit 1
  fi
  if [ ! -e "${LOCAL_LIB}/${LIB_PATH}" ]; then
    echo "[error] missing library: ${LOCAL_LIB}/${LIB_PATH}" >&2
    exit 1
  fi
}

print_linkage_summary() {
  BIN="$1"
  echo "[verify] Runtime linkage (critical deps):"
  ldd "${BIN}" | sed -n '/libbrpc\|liburing\|libprotobuf\|libgflags\|libleveldb\|libsnappy\|libssl\|libcrypto\|libz/p'
}

echo "[info] kernel: $(uname -r)"
echo "[info] root: ${ROOT_DIR}"
echo "[info] project dir: ${SCRIPT_DIR}"
echo "[info] third_party: ${THIRD_PARTY_DIR}"
mkdir -p "${THIRD_PARTY_SRC_DIR}" "${THIRD_PARTY_INSTALL_DIR}"

echo "[step] Install system dependencies"
apt_update_or_die
${SUDO} apt-get install -y \
  build-essential \
  ca-certificates \
  cmake \
  make \
  perl \
  python3 \
  git \
  pkg-config \
  linux-libc-dev \
  curl

ensure_dir "${THIRD_PARTY_SRC_DIR}"
ensure_dir "${THIRD_PARTY_INSTALL_DIR}"
ensure_dir "${BUILD_DIR}"
ensure_dir "${LOCAL_BIN}"

export PATH="${LOCAL_BIN}:${PATH}"
export CPATH="${LOCAL_INC}:${CPATH:-}"
export LIBRARY_PATH="${LOCAL_LIB}:${LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="${LOCAL_LIB}:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="${LOCAL_PKGCFG}:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="${THIRD_PARTY_INSTALL_DIR}:${CMAKE_PREFIX_PATH:-}"

echo "[step] Build/install zlib (${ZLIB_VERSION})"
download_tarball \
  "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz" \
  "${THIRD_PARTY_SRC_DIR}/zlib-${ZLIB_VERSION}.tar.gz"
extract_strip1 "${THIRD_PARTY_SRC_DIR}/zlib-${ZLIB_VERSION}.tar.gz" "${ZLIB_DIR}"
(
  cd "${ZLIB_DIR}"
  ./configure --prefix="${THIRD_PARTY_INSTALL_DIR}"
  make -j"${JOBS}"
  make install
)

echo "[step] Build/install OpenSSL (${OPENSSL_VERSION})"
download_tarball \
  "https://www.openssl.org/source/old/1.1.1/${OPENSSL_VERSION}.tar.gz" \
  "${THIRD_PARTY_SRC_DIR}/${OPENSSL_VERSION}.tar.gz"
extract_strip1 "${THIRD_PARTY_SRC_DIR}/${OPENSSL_VERSION}.tar.gz" "${OPENSSL_DIR}"
(
  cd "${OPENSSL_DIR}"
  ./config --prefix="${THIRD_PARTY_INSTALL_DIR}" --openssldir="${THIRD_PARTY_INSTALL_DIR}/ssl" shared zlib
  make -j"${JOBS}"
  make install_sw
)

echo "[step] Build/install gflags (${GFLAGS_VERSION})"
clone_or_update "https://github.com/gflags/gflags.git" "${GFLAGS_DIR}" "${GFLAGS_VERSION}"
rm -rf "${BUILD_DIR}/gflags"
cmake -S "${GFLAGS_DIR}" -B "${BUILD_DIR}/gflags" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${THIRD_PARTY_INSTALL_DIR}" \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_STATIC_LIBS=ON \
  -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}/gflags" -j"${JOBS}"
cmake --install "${BUILD_DIR}/gflags"

echo "[step] Build/install protobuf (${PROTOBUF_VERSION})"
clone_or_update "https://github.com/protocolbuffers/protobuf.git" "${PROTOBUF_DIR}" "${PROTOBUF_VERSION}"
rm -rf "${BUILD_DIR}/protobuf"
cmake -S "${PROTOBUF_DIR}/cmake" -B "${BUILD_DIR}/protobuf" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${THIRD_PARTY_INSTALL_DIR}" \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=ON \
  -DZLIB_ROOT="${THIRD_PARTY_INSTALL_DIR}" \
  -DCMAKE_PREFIX_PATH="${THIRD_PARTY_INSTALL_DIR}"
cmake --build "${BUILD_DIR}/protobuf" -j"${JOBS}"
cmake --install "${BUILD_DIR}/protobuf"

echo "[step] Build/install snappy (${SNAPPY_VERSION})"
download_tarball \
  "https://github.com/google/snappy/archive/refs/tags/${SNAPPY_VERSION}.tar.gz" \
  "${THIRD_PARTY_SRC_DIR}/snappy-${SNAPPY_VERSION}.tar.gz"
extract_strip1 "${THIRD_PARTY_SRC_DIR}/snappy-${SNAPPY_VERSION}.tar.gz" "${SNAPPY_DIR}"
rm -rf "${BUILD_DIR}/snappy"
cmake -S "${SNAPPY_DIR}" -B "${BUILD_DIR}/snappy" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${THIRD_PARTY_INSTALL_DIR}" \
  -DCMAKE_CXX_FLAGS="-Wno-error -Wno-error=sign-compare" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -Wno-error -Wno-error=sign-compare" \
  -DBUILD_SHARED_LIBS=ON \
  -DSNAPPY_BUILD_TESTS=OFF \
  -DSNAPPY_BUILD_BENCHMARKS=OFF
cmake --build "${BUILD_DIR}/snappy" -j"${JOBS}"
cmake --install "${BUILD_DIR}/snappy"

echo "[step] Build/install leveldb (${LEVELDB_VERSION})"
download_tarball \
  "https://github.com/google/leveldb/archive/refs/tags/${LEVELDB_VERSION}.tar.gz" \
  "${THIRD_PARTY_SRC_DIR}/leveldb-${LEVELDB_VERSION}.tar.gz"
extract_strip1 "${THIRD_PARTY_SRC_DIR}/leveldb-${LEVELDB_VERSION}.tar.gz" "${LEVELDB_DIR}"
rm -rf "${BUILD_DIR}/leveldb"
cmake -S "${LEVELDB_DIR}" -B "${BUILD_DIR}/leveldb" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${THIRD_PARTY_INSTALL_DIR}" \
  -DBUILD_SHARED_LIBS=ON \
  -DHAVE_TCMALLOC=0 \
  -DLEVELDB_BUILD_TESTS=OFF \
  -DLEVELDB_BUILD_BENCHMARKS=OFF \
  -DCMAKE_PREFIX_PATH="${THIRD_PARTY_INSTALL_DIR}"
cmake --build "${BUILD_DIR}/leveldb" -j"${JOBS}"
cmake --install "${BUILD_DIR}/leveldb"

echo "[step] Build/install liburing from source"
if [ ! -d "${LIBURING_DIR}/.git" ]; then
  git clone https://github.com/axboe/liburing.git "${LIBURING_DIR}"
fi
(
  cd "${LIBURING_DIR}"
  ./configure --prefix="${THIRD_PARTY_INSTALL_DIR}"
  make -C src -j"${JOBS}"
  make -C src install
)

echo "[step] Validate local third-party prerequisites"
check_local_dep "zlib.h" "libz.so"
check_local_dep "openssl/ssl.h" "libssl.so"
check_local_dep "gflags/gflags.h" "libgflags.so"
check_local_dep "google/protobuf/message.h" "libprotobuf.so"
check_local_dep "snappy.h" "libsnappy.so"
check_local_dep "leveldb/db.h" "libleveldb.so"
check_local_dep "liburing.h" "liburing.so"
if [ ! -x "${LOCAL_BIN}/protoc" ]; then
  echo "[error] missing protoc: ${LOCAL_BIN}/protoc" >&2
  exit 1
fi

echo "[step] Clone/update and build brpc (${BRPC_BRANCH})"
if [ ! -d "${BRPC_DIR}/.git" ]; then
  git clone --branch "${BRPC_BRANCH}" "${BRPC_REPO}" "${BRPC_DIR}"
else
  (
    cd "${BRPC_DIR}"
    git fetch origin "${BRPC_BRANCH}"
    git checkout "${BRPC_BRANCH}"
    git pull --ff-only origin "${BRPC_BRANCH}"
  )
fi
(
  cd "${BRPC_DIR}"
  sh config_brpc.sh \
    --headers="${THIRD_PARTY_INSTALL_DIR}/include" \
    --libs="${THIRD_PARTY_INSTALL_DIR}/lib"
  make -j"${JOBS}"
)

echo "[step] Build server"
cmake -S "${SCRIPT_DIR}" -B "${SERVER_BUILD_DIR}" \
  -DBRPC_ROOT="${BRPC_DIR}/output" \
  -DTHIRD_PARTY_ROOT="${THIRD_PARTY_INSTALL_DIR}"
cmake --build "${SERVER_BUILD_DIR}" -j"${JOBS}"
ln -sfn "build/compile_commands.json" "${SCRIPT_DIR}/compile_commands.json"

echo "[verify] CMake resolved paths:"
sed -n '/BRPC_INCLUDE_DIR\|BRPC_LIBRARY\|LIBURING_INCLUDE_DIR\|LIBURING_LIBRARY\|PROTOBUF_INCLUDE_DIR\|PROTOBUF_LIBRARY\|GFLAGS_INCLUDE_DIR\|GFLAGS_LIBRARY/p' \
  "${SERVER_BUILD_DIR}/CMakeCache.txt"
print_linkage_summary "${SERVER_BUILD_DIR}/file_read_server"

echo "[done] server ready:"
echo "       ${SERVER_BUILD_DIR}/file_read_server"
echo "[done] third-party install:"
echo "       ${THIRD_PARTY_INSTALL_DIR}"
