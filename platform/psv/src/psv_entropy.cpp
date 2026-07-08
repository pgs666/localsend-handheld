#include <mbedtls/entropy.h>

#include <algorithm>
#include <cstddef>
#include <unistd.h>

extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
  (void)data;

  size_t written = 0;
  while (written < len) {
    const size_t chunk = std::min<size_t>(len - written, 256);
    if (getentropy(output + written, chunk) != 0) {
      *olen = written;
      return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    written += chunk;
  }

  *olen = written;
  return 0;
}
