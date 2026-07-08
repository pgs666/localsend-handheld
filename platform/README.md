# Platform Layer

Current targets:

- `desktop`: POSIX sockets, std threads, filesystem paths, protocol tests.
- `switch`: libnx console `.nro` with HTTP receive MVP, UDP announcements, and inbox at `sdmc:/switch/localsend/inbox/`.
- `psv`: VitaSDK smoke `.vpk`; protocol receive/send still needs implementation.

Planned handheld paths:

- Switch config: `sdmc:/switch/localsend/config.json`
- Switch inbox: `sdmc:/switch/localsend/inbox/`
- PSV config: `ux0:data/localsend/config.json`
- PSV inbox: `ux0:data/localsend/inbox/`

The portable core protocol code stays free of borealis/libnx/VitaSDK includes.
