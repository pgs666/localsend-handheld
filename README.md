# LocalSend Handheld

C++17/CMake LocalSend protocol-compatible homebrew client prototype for Nintendo
Switch and PlayStation Vita, with a desktop target for protocol development and
tests.

Initial scope:

- LocalSend protocol v2.1-compatible HTTP mode, with HTTPS support in desktop core and Switch shared UI builds.
- Desktop protocol prototype for protocol development and tests.
- Nintendo Switch `.nro` HTTPS receive MVP.
- PlayStation Vita `.vpk` borealis/GXM receive MVP.
- Official LocalSend peers must disable Encryption for PSV and HTTP-only desktop runs. Switch builds now use HTTPS by default, but still need renewed hardware verification after the shared UI migration.

## Layout

- `include/localsend`, `src/core`: protocol DTOs, JSON, UDP discovery, HTTP send/receive.
- `app`: desktop prototype entry point and shared borealis handheld UI shell.
- `platform`: Switch libnx and PSV VitaSDK targets.
- `third_party`: reserved for pinned vendored dependencies such as Mongoose, yyjson, borealis.

## Build

```bash
cmake -S . -B build -DPLATFORM_DESKTOP=ON -DLOCALSEND_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Desktop CMake options:

```bash
cmake -S . -B build-switch -DPLATFORM_DESKTOP=OFF -DPLATFORM_SWITCH=ON
cmake -S . -B build-psv -DPLATFORM_DESKTOP=OFF -DPLATFORM_PSV=ON
```

For local artifact builds, use the helper script:

```bash
./scripts/build_handheld_artifacts.sh desktop
USE_CONTAINER=1 ./scripts/build_handheld_artifacts.sh both
```

`USE_CONTAINER=1` builds the handheld artifacts with Docker or Podman SDK
containers:

- Switch: `devkitpro/devkita64`, producing `build/artifacts/local/localsend-handheld.nro`.
- PSV: `vitasdk/vitasdk`, producing `build/artifacts/local/localsend-handheld.vpk`.

Switch builds default to the host CPU count for parallelism. Override this when
needed:

```bash
SWITCH_BUILD_JOBS=6 USE_CONTAINER=1 ./scripts/build_handheld_artifacts.sh switch
```

If the build server needs a LAN SOCKS proxy for Docker Hub or GitHub release
downloads, export proxy variables before running the helper. They are passed
through into the SDK containers:

```bash
export ALL_PROXY=socks5h://192.168.31.228:7890
export HTTPS_PROXY="$ALL_PROXY"
export HTTP_PROXY="$ALL_PROXY"
export NO_PROXY=localhost,127.0.0.1,::1,192.168.31.0/24
USE_CONTAINER=1 ./scripts/build_handheld_artifacts.sh both
```

Docker daemon image pulls also need a daemon-level proxy when direct Docker Hub
access is blocked.

The repository CI also builds handheld artifacts directly with platform SDK containers:

- Switch: `platform/switch/build-ui/localsend-handheld.nro` via `devkitpro/devkita64`.
- PSV: `platform/psv/build/localsend-handheld.vpk` via `vitasdk/vitasdk`, including `sce_sys/icon0.png`.

Local platform builds require devkitPro/libnx or VitaSDK installed.

## Deploy

After a successful local or CI build, deploy/probe devices with:

```bash
SWITCH_FTP_PASS=... ARTIFACT_DIR=build/artifacts/local ./scripts/deploy_handheld_artifacts.sh switch
ARTIFACT_DIR=build/artifacts/local ./scripts/deploy_handheld_artifacts.sh psv
./scripts/deploy_handheld_artifacts.sh probe
SWITCH_FTP_PASS=... ./scripts/deploy_handheld_artifacts.sh logs
```

The script defaults to Switch FTP `192.168.31.48:5000` with user `switch`, with
the password supplied via `SWITCH_FTP_PASS`, and PSV FTP `192.168.31.6:1337`
without credentials. Override these with `SWITCH_HOST`, `SWITCH_FTP_PORT`,
`PSV_HOST`, or `PSV_FTP_PORT`.

## Desktop Prototype

Print a multicast-compatible JSON payload:

```bash
./build/localsend-desktop info "Switch Prototype"
```

Receive files over HTTP:

```bash
./build/localsend-desktop serve inbox 53317 "Handheld"
```

Send one or more files to an HTTP peer:

```bash
./build/localsend-desktop send 192.168.1.20 53317 ./photo.jpg ./notes.txt --alias "Handheld"
```

Listen for UDP multicast announcements:

```bash
./build/localsend-desktop discover 1000
```

The prototype implements the v2 routes required for the HTTP MVP:

- `GET /api/localsend/v2/info`
- `POST /api/localsend/v2/register`
- `POST /api/localsend/v2/prepare-upload`
- `POST /api/localsend/v2/upload?sessionId=...&fileId=...&token=...`
- `POST /api/localsend/v2/cancel?sessionId=...`

It also accepts the legacy v1 route names used by some official clients during
manual IP probing:

- `GET /api/localsend/v1/info`
- `POST /api/localsend/v1/register`
- `POST /api/localsend/v1/send-request`
- `POST /api/localsend/v1/send?fileId=...&token=...`
- `POST /api/localsend/v1/cancel`

## Current Limits

- Desktop core and Switch shared UI builds support HTTPS with a persistent self-signed certificate. PSV remains HTTP-only until its mbedTLS entropy source is wired and verified.
- No PIN, text messages, recursive folders, `/prepare-download`, or `/download`.
- HTTP upload sends and receives files serially with fixed 64 KiB streaming buffers.
- Switch now has a shared borealis UI shell build that starts the portable HTTPS receive service and discovery announcements. The older console bring-up source is still kept under `platform/switch/src/main.cpp` for reference while the UI path stabilizes, but it is not part of the default NRO build.
- PSV uses the same shared borealis UI shell, exposes HTTP receive routes on `ux0:data/localsend/inbox/`, and periodically announces itself for discovery. The portable send core is compiled for PSV, but the PSV UI still needs a device picker and file browser before user-driven sending is available.
- borealis handheld UI is still a shared status screen; the full device list, transfer list, file picker, and settings pages are pending.
