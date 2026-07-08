# Platform Layer

Current targets:

- `desktop`: POSIX sockets, std threads, filesystem paths, protocol tests.
- `switch`: libnx console `.nro` with HTTP/HTTPS receive MVP, HTTPS send bring-up path, UDP announcements, and inbox at `sdmc:/switch/localsend/inbox/`.
- `psv`: VitaSDK borealis/GXM `.vpk` with HTTP receive, periodic discovery announce, and inbox at `ux0:data/localsend/inbox/`. The portable send and HTTPS core compile for PSV; user-driven send UI and runtime HTTPS enablement are still pending.

Planned handheld paths:

- Switch config: `sdmc:/switch/localsend/config.json`
- Switch inbox: `sdmc:/switch/localsend/inbox/`
- PSV config: `ux0:data/localsend/config.json`
- PSV inbox: `ux0:data/localsend/inbox/`

The portable core protocol code stays free of borealis/libnx/VitaSDK includes.

Switch HTTPS currently uses an embedded development certificate in the console
MVP. The portable core already has persistent `cert.pem` / `key.pem` generation;
the platform app still needs to load that identity instead of embedding PEM data.
