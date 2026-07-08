# Platform Layer

Planned adapters:

- `desktop`: POSIX sockets, std threads, filesystem paths for protocol tests.
- `switch`: libnx sockets, `sdmc:/switch/localsend/` config and inbox paths.
- `psv`: vitasdk sockets, `ux0:data/localsend/` config and inbox paths.

The core protocol code is intentionally free of borealis/libnx/vitasdk includes.

