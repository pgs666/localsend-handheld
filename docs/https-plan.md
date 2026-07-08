# HTTPS compatibility plan

LocalSend encryption uses HTTPS with a self-signed certificate. The upstream app
generates an RSA key pair, creates a 10-year self-signed certificate with
`CN=LocalSend User`, and advertises the SHA-256 hash of the certificate DER as
the device fingerprint.

Observed upstream behavior:

- `protocol` is `https` in discovery/register/info when encryption is enabled.
- `fingerprint` is the lowercase SHA-256 hex digest of the DER certificate.
- TLS clients disable normal CA verification because peers use self-signed
  certificates.
- The HTTP API routes and JSON DTOs are otherwise the same as HTTP mode.

Implementation sequence:

1. Keep HTTP mode as a debug fallback.
2. Add dependency-free certificate fingerprint parsing in core. Done.
3. Vendor mbedTLS at a pinned version and build it for desktop, Switch, and PSV. Done for CI checkout and desktop core linkage.
4. Add a reusable TLS stream wrapper around existing TCP sockets. Done for desktop loopback tests.
5. Generate or load `cert.pem` and `key.pem` per platform. Done in portable core.
6. Wrap the existing HTTP server/client stream operations with mbedTLS. Done for desktop core.
7. Advertise `protocol: "https"` and the certificate fingerprint. Done for desktop core.
8. Verify peer fingerprints when discovery/register/info provided one. Done for desktop core.
9. Link mbedTLS into Switch and PSV package builds. Done for Switch NRO; PSV still needs protocol integration.
10. Add UI/config controls to enable HTTPS on handheld targets. Switch currently advertises HTTPS in the console MVP.
11. Replace the temporary embedded Switch certificate with the portable persistent identity loader.

Expected platform paths:

- Switch: `sdmc:/switch/localsend/cert.pem`, `sdmc:/switch/localsend/key.pem`.
- PSV: `ux0:data/localsend/cert.pem`, `ux0:data/localsend/key.pem`.

Current handheld status:

- Desktop core supports HTTPS send and receive in tests.
- Switch console MVP supports HTTPS target probing, HTTPS file send, HTTPS discovery, and same-port HTTP/HTTPS receive.
- Switch HTTPS receive still uses an embedded development certificate; persistent platform certificate loading is the next cleanup step.
- PSV remains a packaging smoke target.
