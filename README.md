# LocalSend Handheld

C++17/CMake LocalSend protocol-compatible homebrew client prototype for Nintendo
Switch and PlayStation Vita, with a desktop target for protocol development and
tests.

Initial scope:

- LocalSend protocol v2.1-compatible HTTP mode.
- Desktop protocol prototype first.
- Switch and PSV platform targets staged behind CMake options.
- Official LocalSend peers must disable Encryption until HTTPS support lands.

## Layout

- `include/localsend`, `src/core`: protocol DTOs, JSON, UDP discovery, HTTP send/receive.
- `app`: desktop prototype entry point now; borealis handheld UI later.
- `platform`: reserved for Switch libnx, PSV vitasdk, and desktop adapters.
- `third_party`: reserved for pinned vendored dependencies such as Mongoose, yyjson, borealis.

## Build

```bash
cmake -S . -B build -DPLATFORM_DESKTOP=ON -DLOCALSEND_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Staged platform switches:

```bash
cmake -S . -B build-switch -DPLATFORM_DESKTOP=OFF -DPLATFORM_SWITCH=ON
cmake -S . -B build-psv -DPLATFORM_DESKTOP=OFF -DPLATFORM_PSV=ON
```

The Switch and PSV options currently define target boundaries only; actual `.nro`
and `.vpk` packaging still needs the platform SDK integration.

## Desktop Prototype

Print a multicast-compatible JSON payload:

```bash
./build/localsend-desktop info "Switch Prototype"
```

Receive files over HTTP:

```bash
./build/localsend-desktop serve inbox 53317 "Handheld"
```

Send one file to an HTTP peer:

```bash
./build/localsend-desktop send 192.168.1.20 53317 ./photo.jpg "Handheld"
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

## Current Limits

- HTTP only; official LocalSend peers must turn off Encryption.
- No PIN, text messages, recursive folders, `/prepare-download`, or `/download`.
- HTTP upload sends and receives file bodies with fixed 64 KiB streaming buffers.
- Switch/PSV UI and package generation are not implemented yet.
