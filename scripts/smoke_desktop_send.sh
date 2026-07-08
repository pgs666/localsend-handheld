#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESKTOP_BIN="${DESKTOP_BIN:-${ROOT_DIR}/build/localsend-desktop}"
HOST=""
PORT="53317"
HTTPS="0"
FINGERPRINT=""
ALIAS="LocalSend Desktop Smoke"
FILE=""
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-180}"

usage() {
  cat <<EOF
Usage: $0 --host <ip> [--port 53317] [--http|--https] [--fingerprint sha256] [--file path] [--alias name]

Examples:
  $0 --host 192.168.31.150 --http
  $0 --host 192.168.31.240 --https
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      HOST="${2:-}"
      shift 2
      ;;
    --port)
      PORT="${2:-}"
      shift 2
      ;;
    --http)
      HTTPS="0"
      shift
      ;;
    --https)
      HTTPS="1"
      shift
      ;;
    --fingerprint)
      FINGERPRINT="${2:-}"
      shift 2
      ;;
    --file)
      FILE="${2:-}"
      shift 2
      ;;
    --alias)
      ALIAS="${2:-}"
      shift 2
      ;;
    -h|--help|help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${HOST}" ]]; then
  usage >&2
  exit 2
fi
if [[ ! -x "${DESKTOP_BIN}" ]]; then
  echo "Missing executable: ${DESKTOP_BIN}" >&2
  echo "Build it with: cmake --build build --target localsend-desktop" >&2
  exit 1
fi

SCHEME="http"
CURL_TLS=()
if [[ "${HTTPS}" == "1" ]]; then
  SCHEME="https"
  CURL_TLS=(-k)
fi

INFO_URL="${SCHEME}://${HOST}:${PORT}/api/localsend/v2/info"
INFO="$(curl --connect-timeout 3 --max-time 8 -fsS "${CURL_TLS[@]}" "${INFO_URL}")"
echo "Peer info: ${INFO}"

if [[ "${HTTPS}" == "1" && -z "${FINGERPRINT}" ]]; then
  FINGERPRINT="$(printf '%s\n' "${INFO}" | sed -n 's/.*"fingerprint":"\([^"]*\)".*/\1/p')"
  if [[ -z "${FINGERPRINT}" ]]; then
    echo "Could not extract fingerprint from /info." >&2
    exit 1
  fi
fi

TEMP_FILE=""
if [[ -z "${FILE}" ]]; then
  TEMP_FILE="$(mktemp /tmp/localsend-handheld-smoke.XXXXXX.txt)"
  printf 'localsend desktop smoke %s\n' "$(date -Is)" > "${TEMP_FILE}"
  FILE="${TEMP_FILE}"
fi

cleanup() {
  if [[ -n "${TEMP_FILE}" ]]; then
    rm -f "${TEMP_FILE}"
  fi
}
trap cleanup EXIT

CMD=("${DESKTOP_BIN}" send "${HOST}" "${PORT}" "${FILE}" --alias "${ALIAS}")
if [[ "${HTTPS}" == "1" ]]; then
  CMD+=(--https --fingerprint "${FINGERPRINT}")
fi

echo "Sending ${FILE} to ${SCHEME}://${HOST}:${PORT}"
LOCALSEND_DEBUG_SEND=1 timeout "${TIMEOUT_SECONDS}" "${CMD[@]}"
