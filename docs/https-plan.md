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
2. Add dependency-free certificate fingerprint parsing in core.
3. Vendor mbedTLS at a pinned version and build it for desktop, Switch, and PSV.
4. Generate or load `cert.pem` and `key.pem` per platform.
5. Wrap the existing HTTP server/client stream operations with mbedTLS.
6. Advertise `protocol: "https"` and the certificate fingerprint.
7. Verify peer fingerprints when discovery/register/info provided one.

Expected platform paths:

- Switch: `sdmc:/switch/localsend/cert.pem`, `sdmc:/switch/localsend/key.pem`.
- PSV: `ux0:data/localsend/cert.pem`, `ux0:data/localsend/key.pem`.
