#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARTIFACT_DIR="${ARTIFACT_DIR:-}"

SWITCH_HOST="${SWITCH_HOST:-192.168.31.48}"
SWITCH_FTP_PORT="${SWITCH_FTP_PORT:-5000}"
SWITCH_FTP_USER="${SWITCH_FTP_USER:-switch}"
SWITCH_FTP_PASS="${SWITCH_FTP_PASS:-}"
SWITCH_HTTP_PORT="${SWITCH_HTTP_PORT:-53317}"
SWITCH_NRO_PATH="${SWITCH_NRO_PATH:-sdmc:/switch/localsend-handheld/localsend-handheld.nro}"
SWITCH_LOG_PATH="${SWITCH_LOG_PATH:-sdmc:/switch/localsend/localsend-borealis.log}"

PSV_HOST="${PSV_HOST:-192.168.31.6}"
PSV_FTP_PORT="${PSV_FTP_PORT:-1337}"
PSV_HTTP_PORT="${PSV_HTTP_PORT:-53317}"
PSV_VPK_PATH="${PSV_VPK_PATH:-ux0:/data/localsend-handheld.vpk}"
PSV_LOG_PATH="${PSV_LOG_PATH:-ux0:/data/localsend-borealis.log}"

usage() {
  cat <<EOF
Usage: $0 [switch|psv|both|probe|logs]

Environment overrides:
  ARTIFACT_DIR       Directory containing localsend-handheld.nro/.vpk.
  SWITCH_HOST        Default: ${SWITCH_HOST}
  SWITCH_FTP_PORT    Default: ${SWITCH_FTP_PORT}
  SWITCH_FTP_PASS    Required for Switch FTP.
  PSV_HOST           Default: ${PSV_HOST}
  PSV_FTP_PORT       Default: ${PSV_FTP_PORT}

Examples:
  ARTIFACT_DIR=build/artifacts/28927001724 $0 both
  $0 probe
  $0 logs
EOF
}

latest_artifact_dir() {
  find "${ROOT_DIR}/build/artifacts" -mindepth 1 -maxdepth 1 -type d -printf '%T@ %p\n' 2>/dev/null \
    | sort -nr \
    | awk 'NR == 1 { print $2 }'
}

resolve_artifact_dir() {
  if [[ -n "${ARTIFACT_DIR}" ]]; then
    if [[ "${ARTIFACT_DIR}" != /* ]]; then
      ARTIFACT_DIR="${ROOT_DIR}/${ARTIFACT_DIR}"
    fi
  else
    ARTIFACT_DIR="$(latest_artifact_dir)"
  fi

  if [[ -z "${ARTIFACT_DIR}" || ! -d "${ARTIFACT_DIR}" ]]; then
    echo "No artifact directory found. Set ARTIFACT_DIR." >&2
    exit 1
  fi
}

ftp_put_switch() {
  local src="$1"
  local dst="$2"
  require_switch_password
  curl --user "${SWITCH_FTP_USER}:${SWITCH_FTP_PASS}" \
    --connect-timeout 8 --max-time 120 --ftp-create-dirs \
    -T "${src}" "ftp://${SWITCH_HOST}:${SWITCH_FTP_PORT}/${dst}"
}

ftp_get_switch() {
  local path="$1"
  require_switch_password
  curl --user "${SWITCH_FTP_USER}:${SWITCH_FTP_PASS}" \
    --connect-timeout 8 --max-time 20 -sS \
    "ftp://${SWITCH_HOST}:${SWITCH_FTP_PORT}/${path}"
}

require_switch_password() {
  if [[ -z "${SWITCH_FTP_PASS}" ]]; then
    echo "Set SWITCH_FTP_PASS for Switch FTP access." >&2
    exit 1
  fi
}

ftp_put_psv() {
  local src="$1"
  local dst="$2"
  curl --connect-timeout 8 --max-time 120 --ftp-create-dirs \
    -T "${src}" "ftp://${PSV_HOST}:${PSV_FTP_PORT}/${dst}"
}

ftp_get_psv() {
  local path="$1"
  curl --connect-timeout 8 --max-time 20 -sS \
    "ftp://${PSV_HOST}:${PSV_FTP_PORT}/${path}"
}

probe_http() {
  local name="$1"
  local host="$2"
  local port="$3"
  echo "== ${name} /info =="
  curl --connect-timeout 3 --max-time 8 -sS \
    "http://${host}:${port}/api/localsend/v2/info" || true
  echo
}

deploy_switch() {
  resolve_artifact_dir
  local nro="${ARTIFACT_DIR}/localsend-handheld.nro"
  [[ -f "${nro}" ]] || { echo "Missing ${nro}" >&2; exit 1; }

  echo "Uploading Switch NRO to ${SWITCH_HOST}:${SWITCH_FTP_PORT}/${SWITCH_NRO_PATH}"
  ftp_put_switch "${nro}" "${SWITCH_NRO_PATH}"
  echo
  ftp_get_switch "$(dirname "${SWITCH_NRO_PATH}")/"
}

deploy_psv() {
  resolve_artifact_dir
  local vpk="${ARTIFACT_DIR}/localsend-handheld.vpk"
  [[ -f "${vpk}" ]] || { echo "Missing ${vpk}" >&2; exit 1; }

  echo "Uploading PSV VPK to ${PSV_HOST}:${PSV_FTP_PORT}/${PSV_VPK_PATH}"
  ftp_put_psv "${vpk}" "${PSV_VPK_PATH}"
  echo
  ftp_get_psv "$(dirname "${PSV_VPK_PATH}")/"
}

show_logs() {
  echo "== Switch log =="
  ftp_get_switch "${SWITCH_LOG_PATH}" | tail -120 || true
  echo
  echo "== PSV log =="
  ftp_get_psv "${PSV_LOG_PATH}" | tail -120 || true
}

cmd="${1:-probe}"
case "${cmd}" in
  switch)
    deploy_switch
    ;;
  psv)
    deploy_psv
    ;;
  both)
    deploy_switch
    deploy_psv
    ;;
  probe)
    probe_http "Switch" "${SWITCH_HOST}" "${SWITCH_HTTP_PORT}"
    probe_http "PSV" "${PSV_HOST}" "${PSV_HTTP_PORT}"
    ;;
  logs)
    show_logs
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
