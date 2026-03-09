#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./scripts/build_gcc_clang.sh [options]

Options:
  --jobs <N>      Parallel build jobs. Default: nproc
  --skip-gcc      Skip gcc build.
  --skip-clang    Skip clang build.
  -h, --help      Show help.

Env:
  BRPC_ROOT                 Default: <repo>/third_party/src/brpc/output
  DEPS_ROOT                 Default: <repo>/third_party/install
  FORCE_STATIC_THIRD_PARTY  Default: ON

Output directories:
  gcc_server/, gcc_client/, clang_server/, clang_client/
EOF
}

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
BUILD_GCC=1
BUILD_CLANG=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --jobs)
      JOBS="${2:-}"
      shift 2
      ;;
    --skip-gcc)
      BUILD_GCC=0
      shift
      ;;
    --skip-clang)
      BUILD_CLANG=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[error] unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "${BUILD_GCC}" -eq 0 && "${BUILD_CLANG}" -eq 0 ]]; then
  echo "[error] both gcc and clang are skipped, nothing to build." >&2
  exit 1
fi

if ! [[ "${JOBS}" =~ ^[0-9]+$ ]] || [[ "${JOBS}" -le 0 ]]; then
  echo "[error] --jobs must be a positive integer, got: ${JOBS}" >&2
  exit 1
fi

BRPC_ROOT="${BRPC_ROOT:-${ROOT_DIR}/third_party/src/brpc/output}"
DEPS_ROOT="${DEPS_ROOT:-${ROOT_DIR}/third_party/install}"
FORCE_STATIC_THIRD_PARTY="${FORCE_STATIC_THIRD_PARTY:-ON}"

ensure_exists() {
  local path="$1"
  local hint="$2"
  if [[ ! -e "${path}" ]]; then
    echo "[error] missing: ${path}" >&2
    echo "[hint] ${hint}" >&2
    exit 1
  fi
}

ensure_exists "${BRPC_ROOT}/include/brpc/server.h" \
  "Run ./bootstrap_server_deps.sh first, or set BRPC_ROOT correctly."
ensure_exists "${DEPS_ROOT}/include/google/protobuf/service.h" \
  "Run ./bootstrap_server_deps.sh first, or set DEPS_ROOT correctly."
ensure_exists "${DEPS_ROOT}/bin/protoc" \
  "Run ./bootstrap_server_deps.sh first, or set DEPS_ROOT correctly."
if [[ "${FORCE_STATIC_THIRD_PARTY}" == "ON" ]]; then
  ensure_exists "${BRPC_ROOT}/lib/libbrpc.a" \
    "Expected static brpc under BRPC_ROOT/lib. Rebuild deps/brpc and retry."
fi

build_one_toolchain() {
  local label="$1"
  local cc="$2"
  local cxx="$3"
  local server_build_dir="${ROOT_DIR}/${label}_server"
  local client_build_dir="${ROOT_DIR}/${label}_client"

  command -v "${cc}" >/dev/null 2>&1 || {
    echo "[error] compiler not found: ${cc}" >&2
    exit 1
  }
  command -v "${cxx}" >/dev/null 2>&1 || {
    echo "[error] compiler not found: ${cxx}" >&2
    exit 1
  }

  echo "[step] clean build dirs (${label})"
  rm -rf "${server_build_dir}" "${client_build_dir}"

  echo "[step] configure server (${label}) -> ${server_build_dir}"
  cmake -S "${ROOT_DIR}/server" -B "${server_build_dir}" \
    -DCMAKE_CXX_COMPILER="${cxx}" \
    -DPROTOC_EXECUTABLE="${DEPS_ROOT}/bin/protoc" \
    -DFORCE_STATIC_THIRD_PARTY="${FORCE_STATIC_THIRD_PARTY}" \
    -DBRPC_ROOT="${BRPC_ROOT}" \
    -DDEPS_ROOT="${DEPS_ROOT}"
  cmake --build "${server_build_dir}" -j"${JOBS}"

  echo "[step] configure client (${label}) -> ${client_build_dir}"
  cmake -S "${ROOT_DIR}/client" -B "${client_build_dir}" \
    -DCMAKE_CXX_COMPILER="${cxx}" \
    -DPROTOC_EXECUTABLE="${DEPS_ROOT}/bin/protoc" \
    -DFORCE_STATIC_THIRD_PARTY="${FORCE_STATIC_THIRD_PARTY}" \
    -DBRPC_ROOT="${BRPC_ROOT}" \
    -DDEPS_ROOT="${DEPS_ROOT}"
  cmake --build "${client_build_dir}" -j"${JOBS}"
}

if [[ "${BUILD_GCC}" -eq 1 ]]; then
  build_one_toolchain "gcc" "gcc" "g++"
fi

if [[ "${BUILD_CLANG}" -eq 1 ]]; then
  build_one_toolchain "clang" "clang" "clang++"
fi

echo "[done] build outputs:"
if [[ "${BUILD_GCC}" -eq 1 ]]; then
  echo "       ${ROOT_DIR}/gcc_server/read_bench_server"
  echo "       ${ROOT_DIR}/gcc_client/read_bench_client"
fi
if [[ "${BUILD_CLANG}" -eq 1 ]]; then
  echo "       ${ROOT_DIR}/clang_server/read_bench_server"
  echo "       ${ROOT_DIR}/clang_client/read_bench_client"
fi
