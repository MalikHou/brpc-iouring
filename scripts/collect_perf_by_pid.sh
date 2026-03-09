#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./scripts/collect_perf_by_pid.sh --pid <PID> [options]

Options:
  --pid <PID>             Target process ID. (required)
  --duration_s <SECONDS>  Collection duration in seconds. (default: 30)
  --all-mode              Use raw events (not forcing :u user-mode suffix).
  -h, --help              Show this help.

Examples:
  ./scripts/collect_perf_by_pid.sh --pid 12345
  ./scripts/collect_perf_by_pid.sh --pid 12345 --duration_s 60
  ./scripts/collect_perf_by_pid.sh --pid 12345 --all-mode
EOF
}

PID=""
DURATION_S=30
# Built-in key indicators for quick bottleneck triage.
EVENTS="task-clock,cycles,instructions,branches,branch-misses,cache-misses,context-switches,cpu-migrations,page-faults,minor-faults,major-faults"
FORCE_USER_MODE=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pid)
      PID="${2:-}"
      shift 2
      ;;
    --duration_s)
      DURATION_S="${2:-}"
      shift 2
      ;;
    --all-mode)
      FORCE_USER_MODE=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "${PID}" ]]; then
  echo "Missing required argument: --pid" >&2
  usage
  exit 1
fi
if ! [[ "${PID}" =~ ^[0-9]+$ ]]; then
  echo "--pid must be an integer, got: ${PID}" >&2
  exit 1
fi
if ! [[ "${DURATION_S}" =~ ^[0-9]+$ ]] || [[ "${DURATION_S}" -le 0 ]]; then
  echo "--duration_s must be a positive integer, got: ${DURATION_S}" >&2
  exit 1
fi
if ! kill -0 "${PID}" 2>/dev/null; then
  echo "Process not found or not accessible: pid=${PID}" >&2
  exit 1
fi
if ! command -v perf >/dev/null 2>&1; then
  echo "perf is not installed or not in PATH" >&2
  exit 1
fi
if ! command -v pidstat >/dev/null 2>&1; then
  echo "pidstat is not installed or not in PATH (sysstat package)" >&2
  exit 1
fi

normalized_events="${EVENTS}"
if [[ "${FORCE_USER_MODE}" -eq 1 ]]; then
  IFS=',' read -r -a raw_events <<< "${EVENTS}"
  normalized_events=""
  for e in "${raw_events[@]}"; do
    e_trimmed="$(echo "${e}" | tr -d '[:space:]')"
    if [[ -z "${e_trimmed}" ]]; then
      continue
    fi
    if [[ "${e_trimmed}" == *:* ]]; then
      out_event="${e_trimmed}"
    else
      out_event="${e_trimmed}:u"
    fi
    if [[ -z "${normalized_events}" ]]; then
      normalized_events="${out_event}"
    else
      normalized_events="${normalized_events},${out_event}"
    fi
  done
fi

duration_ms="$((DURATION_S * 1000))"

echo "timestamp=$(date +%Y%m%d_%H%M%S)"
echo "Collecting for pid=${PID}, duration=${DURATION_S}s ..."
echo "events=${normalized_events}"
echo "force_user_mode=${FORCE_USER_MODE}"
echo "cmdline=$(tr '\0' ' ' < "/proc/${PID}/cmdline" 2>/dev/null || true)"
echo "ps:"
ps -fp "${PID}" || true
echo

echo "=== pidstat (-u -r -w -t) ==="
# One-shot sample at the end of duration_s.
pidstat -u -r -w -t -p "${PID}" "${DURATION_S}" 1 \
  > >(sed 's/^/[pidstat] /') 2>&1 &
pidstat_pid=$!

set +e
echo "=== perf stat ==="
perf stat -p "${PID}" -e "${normalized_events}" --timeout "${duration_ms}" \
  2> >(sed 's/^/[perf] /' >&2)
perf_rc=$?
wait "${pidstat_pid}"
pidstat_rc=$?
set -e

echo "Done."
echo "perf_exit_code=${perf_rc}"
echo "pidstat_exit_code=${pidstat_rc}"

if [[ "${perf_rc}" -ne 0 ]]; then
  echo "Warning: perf exited with code ${perf_rc}" >&2
fi
