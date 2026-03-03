#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  ./scripts/collect_iouring_diag_bundle.sh [options] [-- <custom_monitor_args...>]

Options:
  --host <ip>                 Server host for monitor /vars (default: 10.192.101.15)
  --monitor_port <port>       Monitor HTTP port (default: 8041)
  --service <io_uring|blocking>
                              Service for brpc_latency_monitor (default: io_uring)
  --service_port_map <map>    Port map (default: io_uring:8040,blocking:8042)
  --threads <n>               Monitor threads (default: 32)
  --duration_s <sec>          Test duration (default: 30)
  --timeout_ms <ms>           RPC timeout in monitor (default: 10000)
  --len_bytes <n>             Request bytes (default: 32768)
  --file_size_bytes <n>       File size bound (default: 4294967296)
  --rounds <n>                Number of rounds (default: 1)
  --monitor_bin <path>        Monitor binary (default: ./scripts/brpc_latency_monitor)
  --out_dir <path>            Output directory (default: /tmp/iouring_diag_bundle_<ts>)
  --help                      Show help

Examples:
  ./scripts/collect_iouring_diag_bundle.sh --service io_uring --rounds 10

  ./scripts/collect_iouring_diag_bundle.sh --host 10.192.101.15 --rounds 3 -- \
    --host=10.192.101.15 --service=io_uring --threads=32 --duration_s=30 --timeout_ms=2000
USAGE
}

HOST="10.192.101.15"
MONITOR_PORT="8041"
SERVICE="io_uring"
SERVICE_PORT_MAP="io_uring:8040,blocking:8042"
THREADS="32"
DURATION_S="30"
TIMEOUT_MS="10000"
LEN_BYTES="32768"
FILE_SIZE_BYTES="$((4*1024*1024*1024))"
ROUNDS="1"
MONITOR_BIN="./scripts/brpc_latency_monitor"
OUT_DIR="/tmp/iouring_diag_bundle_$(date +%Y%m%d_%H%M%S)"

CUSTOM_MONITOR_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      HOST="$2"; shift 2 ;;
    --monitor_port)
      MONITOR_PORT="$2"; shift 2 ;;
    --service)
      SERVICE="$2"; shift 2 ;;
    --service_port_map)
      SERVICE_PORT_MAP="$2"; shift 2 ;;
    --threads)
      THREADS="$2"; shift 2 ;;
    --duration_s)
      DURATION_S="$2"; shift 2 ;;
    --timeout_ms)
      TIMEOUT_MS="$2"; shift 2 ;;
    --len_bytes)
      LEN_BYTES="$2"; shift 2 ;;
    --file_size_bytes)
      FILE_SIZE_BYTES="$2"; shift 2 ;;
    --rounds)
      ROUNDS="$2"; shift 2 ;;
    --monitor_bin)
      MONITOR_BIN="$2"; shift 2 ;;
    --out_dir)
      OUT_DIR="$2"; shift 2 ;;
    --help|-h)
      usage; exit 0 ;;
    --)
      shift
      CUSTOM_MONITOR_ARGS=("$@")
      break ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 1 ;;
  esac
done

if [[ ! -x "$MONITOR_BIN" ]]; then
  echo "monitor binary not executable: $MONITOR_BIN" >&2
  exit 1
fi
if [[ "$ROUNDS" -le 0 ]]; then
  echo "--rounds must be > 0" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/summary.txt"
touch "$SUMMARY"

KEY_RE='^file_read_iouring_(entry_total|ops_total|harvest_cqe_total|harvest_wake_eagain_total|harvest_wake_error_total|wait_local_timeout_total|wait_local_eintr_total|wait_local_ewouldblock_total|wait_local_other_error_total|wait_timeout_butex_set_total|wait_orphan_total|wait_orphan_timeout_total|wait_orphan_eintr_total|wait_orphan_other_total|wait_timeout_then_cqe_total|cqe_valid_total|cqe_orphan_total|orphan_recycled_total|orphan_pending_total|reqctx_inflight|reqctx_acquire_total|reqctx_release_total|stale_cqe_total|double_release_total|diag_event_total|diag_log_emitted_total|diag_log_suppressed_total|wait_begin_to_cqe_us_count|wait_end_to_cqe_us_count|wait_timeout_to_cqe_us_count|orphan_mark_to_cqe_us_count)|^bthread_butex_(wait_return_timeout_total|wait_return_timeout_value_match_total|wait_return_timeout_value_mismatch_total|wait_return_success_total|wait_return_ewouldblock_total|wait_return_eintr_total|timer_erase_timeout_total|timer_erase_timeout_value_mismatch_total|diag_log_emitted_total|diag_log_suppressed_total)|^bthread_butex_wait_local_(call_total|success_total|timeout_total|timeout_value_match_total|timeout_value_mismatch_total|ewouldblock_total|eintr_total|other_error_total)|^rpc_server_8040_.*_read_error'

snapshot_vars() {
  local out_file="$1"
  /usr/bin/curl -sS --max-time 5 "http://${HOST}:${MONITOR_PORT}/vars" \
    | rg -e "${KEY_RE}" \
    | awk '{
        key=$1;
        gsub(/\r/, "", key);
        val="";
        for (i=NF; i>=2; --i) {
          gsub(/\r/, "", $i);
          if ($i ~ /^-?[0-9]+([.][0-9]+)?$/) {
            val=$i;
            break;
          }
        }
        if (val != "") {
          print key, val;
        }
      }' \
    | sort >"$out_file"
}

delta_vars() {
  local before_file="$1"
  local after_file="$2"
  local out_file="$3"
  join -a1 -a2 -e0 -o 0,1.2,2.2 "$before_file" "$after_file" \
    | awk '{
        before=$2+0;
        after=$3+0;
        delta=after-before;
        printf "%-80s before=%-12s after=%-12s delta=%s\n", $1, before, after, delta;
      }' \
    >"$out_file"
}

{
  echo "host=$HOST monitor_port=$MONITOR_PORT service=$SERVICE rounds=$ROUNDS"
  echo "start_time=$(date -Iseconds)"
} >>"$SUMMARY"

FLAGS_SNAPSHOT="$OUT_DIR/flags_snapshot.txt"
{
  for p in \
    /flags/read_timeout_ms \
    /flags/iouring_mode \
    /flags/iouring_disable_response_body \
    /flags/bthread_active_task_poll_every_nswitch \
    /flags/bthread_active_task_idle_wait_ns \
    /flags/iouring_diag_enable_key_log \
    /flags/iouring_diag_key_log_interval_ms \
    /flags/bthread_butex_diag_enable_key_log \
    /flags/bthread_butex_diag_log_interval_ms
  do
    echo "=== $p ==="
    /usr/bin/curl -sS --max-time 3 "http://${HOST}:${MONITOR_PORT}${p}" || true
    echo
  done
} >"$FLAGS_SNAPSHOT"

for ((i=1; i<=ROUNDS; ++i)); do
  ROUND_PREFIX="$OUT_DIR/round_${i}"
  BEFORE_VARS="${ROUND_PREFIX}_vars_before.txt"
  AFTER_VARS="${ROUND_PREFIX}_vars_after.txt"
  DELTA_VARS="${ROUND_PREFIX}_vars_delta.txt"
  MONITOR_LOG="${ROUND_PREFIX}_monitor.log"

  snapshot_vars "$BEFORE_VARS"

  if [[ ${#CUSTOM_MONITOR_ARGS[@]} -gt 0 ]]; then
    "$MONITOR_BIN" "${CUSTOM_MONITOR_ARGS[@]}" >"$MONITOR_LOG" 2>&1
  else
    "$MONITOR_BIN" \
      --host="$HOST" \
      --service="$SERVICE" \
      --service_port_map="$SERVICE_PORT_MAP" \
      --threads="$THREADS" \
      --duration_s="$DURATION_S" \
      --timeout_ms="$TIMEOUT_MS" \
      --len_bytes="$LEN_BYTES" \
      --file_size_bytes="$FILE_SIZE_BYTES" \
      --set_log_id=true \
      --set_request_id=true \
      --fail_sample_limit=50 >"$MONITOR_LOG" 2>&1
  fi

  snapshot_vars "$AFTER_VARS"
  delta_vars "$BEFORE_VARS" "$AFTER_VARS" "$DELTA_VARS"

  {
    echo "=== round $i monitor final ==="
    rg "^\[final |^\[final-fail\]" "$MONITOR_LOG" || true
    echo "=== round $i var deltas (key) ==="
    rg -n "wait_local_timeout_total|wait_orphan_total|wait_orphan_timeout_total|wait_timeout_butex_set_total|wait_timeout_then_cqe_total|cqe_orphan_total|diag_log_emitted_total|bthread_butex_wait_return_timeout_total|bthread_butex_wait_return_timeout_value_mismatch_total|bthread_butex_timer_erase_timeout_value_mismatch_total|bthread_butex_wait_local_timeout_total|bthread_butex_wait_local_timeout_value_mismatch_total|rpc_server_8040_.*_read_error" "$DELTA_VARS" || true
    echo
  } >>"$SUMMARY"
done

tar -C "$(dirname "$OUT_DIR")" -czf "${OUT_DIR}.tar.gz" "$(basename "$OUT_DIR")"
echo "bundle_dir=$OUT_DIR"
echo "bundle_tar=${OUT_DIR}.tar.gz"
echo "summary=$SUMMARY"
