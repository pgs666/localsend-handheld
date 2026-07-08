# Platform Layer

Current targets:

- `desktop`: POSIX sockets, std threads, filesystem paths, protocol tests.
- `switch`: libnx console `.nro` with HTTP/HTTPS receive MVP, HTTPS send bring-up path, UDP announcements, and inbox at `sdmc:/switch/localsend/inbox/`.
- `psv`: VitaSDK borealis/GXM `.vpk` with HTTP receive, periodic discovery announce, inbox at `ux0:data/localsend/inbox/`, and outbox at `ux0:data/localsend/outbox/`. The shared UI has temporary outbox send/cancel/peer/file selection actions, while the formal device/file picker flow and runtime HTTPS enablement are still pending.

Planned handheld paths:

- Switch config: `sdmc:/switch/localsend/config.json`
- Switch inbox: `sdmc:/switch/localsend/inbox/`
- Switch outbox: `sdmc:/switch/localsend/outbox/`
- PSV config: `ux0:data/localsend/config.json`
- PSV inbox: `ux0:data/localsend/inbox/`
- PSV outbox: `ux0:data/localsend/outbox/`

The portable core protocol code stays free of borealis/libnx/VitaSDK includes.

Switch HTTPS currently uses an embedded development certificate in the console
MVP. The portable core already has persistent `cert.pem` / `key.pem` generation;
the platform app still needs to load that identity instead of embedding PEM data.
