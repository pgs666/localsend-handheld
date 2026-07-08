#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${ROOT_DIR}/build/local}"
ARTIFACT_DIR="${ARTIFACT_DIR:-${ROOT_DIR}/build/artifacts/local}"
USE_CONTAINER="${USE_CONTAINER:-0}"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-}"
SWITCH_IMAGE="${SWITCH_IMAGE:-devkitpro/devkita64:latest}"
PSV_IMAGE="${PSV_IMAGE:-vitasdk/vitasdk:latest}"
HOST_BUILD_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)"
SWITCH_BUILD_JOBS="${SWITCH_BUILD_JOBS:-${HOST_BUILD_JOBS}}"
PSV_BUILD_JOBS="${PSV_BUILD_JOBS:-${HOST_BUILD_JOBS}}"

usage() {
  cat <<EOF
Usage: $0 [desktop|switch|psv|both|all|clean]

Environment:
  BUILD_ROOT        Build directory root. Default: build/local
  ARTIFACT_DIR      Output artifact directory. Default: build/artifacts/local
  USE_CONTAINER     Use Docker/Podman toolchain containers for Switch/PSV. Default: 0
  CONTAINER_ENGINE  docker or podman. Auto-detected when USE_CONTAINER=1.
  SWITCH_IMAGE      Default: ${SWITCH_IMAGE}
  PSV_IMAGE         Default: ${PSV_IMAGE}
  SWITCH_BUILD_JOBS Parallel jobs for Switch builds. Default: host CPU count (${SWITCH_BUILD_JOBS})
  PSV_BUILD_JOBS    Parallel jobs for PSV builds. Default: host CPU count (${PSV_BUILD_JOBS})

Examples:
  $0 desktop
  DEVKITPRO=/opt/devkitpro $0 switch
  VITASDK=/usr/local/vitasdk $0 psv
  USE_CONTAINER=1 $0 both
  ARTIFACT_DIR=build/artifacts/local USE_CONTAINER=1 $0 all
EOF
}

detect_container_engine() {
  if [[ -n "${CONTAINER_ENGINE}" ]]; then
    return
  fi
  if command -v docker >/dev/null 2>&1; then
    CONTAINER_ENGINE=docker
  elif command -v podman >/dev/null 2>&1; then
    CONTAINER_ENGINE=podman
  else
    echo "USE_CONTAINER=1 requires docker or podman." >&2
    exit 1
  fi
}

container_run() {
  local image="$1"
  shift
  detect_container_engine
  "${CONTAINER_ENGINE}" run --rm \
    -e HTTP_PROXY \
    -e HTTPS_PROXY \
    -e ALL_PROXY \
    -e NO_PROXY \
    -e http_proxy \
    -e https_proxy \
    -e all_proxy \
    -e no_proxy \
    -v "${ROOT_DIR}:/work" \
    -w /work \
    "${image}" \
    bash -lc "$*"
}

prepare_artifacts() {
  mkdir -p "${ARTIFACT_DIR}"
}

build_desktop() {
  cmake -S "${ROOT_DIR}" -B "${BUILD_ROOT}/desktop" \
    -DPLATFORM_DESKTOP=ON \
    -DLOCALSEND_BUILD_TESTS=ON
  cmake --build "${BUILD_ROOT}/desktop" --parallel
  ctest --test-dir "${BUILD_ROOT}/desktop" --output-on-failure
}

build_switch_host() {
  if [[ -z "${DEVKITPRO:-}" ]]; then
    echo "Set DEVKITPRO or use USE_CONTAINER=1 for Switch builds." >&2
    exit 1
  fi
  cmake -S "${ROOT_DIR}/platform/switch" -B "${BUILD_ROOT}/switch"
  cmake --build "${BUILD_ROOT}/switch" --parallel "${SWITCH_BUILD_JOBS}"
  cp "${BUILD_ROOT}/switch/localsend-handheld.nro" "${ARTIFACT_DIR}/"
}

build_switch_container() {
  container_run "${SWITCH_IMAGE}" "
    set -euo pipefail
    curl -L -o /tmp/libuam.pkg.tar.zst \
      https://github.com/xfangfang/wiliwili/releases/download/v0.1.0/libuam-f8c9eef01ffe06334d530393d636d69e2b52744b-1-any.pkg.tar.zst
    dkp-pacman -U --noconfirm /tmp/libuam.pkg.tar.zst
    cmake -S platform/switch -B build/local/switch
    cmake --build build/local/switch --parallel \"${SWITCH_BUILD_JOBS}\"
  "
  cp "${BUILD_ROOT}/switch/localsend-handheld.nro" "${ARTIFACT_DIR}/"
}

build_switch() {
  prepare_artifacts
  if [[ "${USE_CONTAINER}" == "1" ]]; then
    build_switch_container
  else
    build_switch_host
  fi
}

build_psv_host() {
  if [[ -z "${VITASDK:-}" ]]; then
    echo "Set VITASDK or use USE_CONTAINER=1 for PSV builds." >&2
    exit 1
  fi
  cmake -S "${ROOT_DIR}/platform/psv" -B "${BUILD_ROOT}/psv"
  cmake --build "${BUILD_ROOT}/psv" --parallel "${PSV_BUILD_JOBS}"
  cp "${BUILD_ROOT}/psv/localsend-handheld.vpk" "${ARTIFACT_DIR}/"
}

build_psv_container() {
  container_run "${PSV_IMAGE}" "
    set -euo pipefail
    cmake -S platform/psv -B build/local/psv
    cmake --build build/local/psv --parallel \"${PSV_BUILD_JOBS}\"
  "
  cp "${BUILD_ROOT}/psv/localsend-handheld.vpk" "${ARTIFACT_DIR}/"
}

build_psv() {
  prepare_artifacts
  if [[ "${USE_CONTAINER}" == "1" ]]; then
    build_psv_container
  else
    build_psv_host
  fi
}

clean() {
  rm -rf "${BUILD_ROOT}" "${ARTIFACT_DIR}"
}

cmd="${1:-all}"
case "${cmd}" in
  desktop)
    build_desktop
    ;;
  switch)
    build_switch
    ;;
  psv)
    build_psv
    ;;
  both)
    build_switch
    build_psv
    ;;
  all)
    build_desktop
    build_switch
    build_psv
    ;;
  clean)
    clean
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac

if [[ -d "${ARTIFACT_DIR}" ]]; then
  find "${ARTIFACT_DIR}" -maxdepth 1 -type f -printf '%p %s bytes\n' | sort
fi
