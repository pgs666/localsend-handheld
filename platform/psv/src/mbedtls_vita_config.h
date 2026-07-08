#pragma once

/* VitaSDK is neither a Unix nor Windows target from mbedTLS' platform probes.
 * Keep the first PSV smoke focused on compile/link viability; platform entropy
 * and HTTPS runtime wiring will be supplied explicitly before TLS is enabled.
 */
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_TIMING_C

#define MBEDTLS_NO_PLATFORM_ENTROPY
