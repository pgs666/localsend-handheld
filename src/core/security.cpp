#include "localsend/security.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#if MBEDTLS_VERSION_NUMBER < 0x03000000
#include <mbedtls/bignum.h>
#endif

#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace localsend {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

std::uint32_t rotate_right(std::uint32_t value, int count) {
  return (value >> count) | (value << (32 - count));
}

std::string hex_encode(const std::array<std::uint8_t, 32>& bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

std::vector<std::uint8_t> base64_decode(const std::string& input) {
  std::vector<std::uint8_t> out;
  int accumulator = 0;
  int bits = -8;
  bool padded = false;

  for (const char c : input) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      continue;
    }
    if (c == '=') {
      padded = true;
      continue;
    }
    if (padded) {
      throw std::runtime_error("invalid base64 padding");
    }
    const int value = base64_value(c);
    if (value < 0) {
      throw std::runtime_error("invalid base64 data");
    }
    accumulator = (accumulator << 6) | value;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }
  return out;
}

std::string pem_body(const std::string& pem) {
  const std::string begin = "-----BEGIN CERTIFICATE-----";
  const std::string end = "-----END CERTIFICATE-----";
  const std::size_t begin_pos = pem.find(begin);
  const std::size_t end_pos = pem.find(end);
  if (begin_pos == std::string::npos || end_pos == std::string::npos || end_pos <= begin_pos) {
    throw std::runtime_error("missing certificate PEM markers");
  }
  return pem.substr(begin_pos + begin.size(), end_pos - (begin_pos + begin.size()));
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open " + path.string());
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write " + path.string());
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

TlsIdentity identity_from_pem(std::string certificate_pem, std::string private_key_pem) {
  TlsIdentity identity;
  identity.certificate_pem = std::move(certificate_pem);
  identity.private_key_pem = std::move(private_key_pem);
  identity.fingerprint = certificate_fingerprint_from_pem(identity.certificate_pem);
  return identity;
}

std::string mbedtls_error_text(const char* operation, int result) {
  return std::string(operation) + " failed: " + std::to_string(result);
}

TlsIdentity generate_tls_identity() {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate;

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&certificate);

  auto cleanup = [&]() {
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  };

  const char* personalization = "localsend-handheld-cert";
  int result = mbedtls_ctr_drbg_seed(&ctr_drbg,
                                     mbedtls_entropy_func,
                                     &entropy,
                                     reinterpret_cast<const unsigned char*>(personalization),
                                     std::strlen(personalization));
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("RNG seed", result));
  }

  result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("key setup", result));
  }

  result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                               mbedtls_pk_ec(key),
                               mbedtls_ctr_drbg_random,
                               &ctr_drbg);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("EC key generation", result));
  }

  unsigned char serial[16] = {};
  result = mbedtls_ctr_drbg_random(&ctr_drbg, serial, sizeof(serial));
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate serial generation", result));
  }
  serial[0] &= 0x7f;
  if (serial[0] == 0) {
    serial[0] = 1;
  }

  mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&certificate, &key);
  mbedtls_x509write_crt_set_issuer_key(&certificate, &key);

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  result = mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial));
#else
  mbedtls_mpi serial_mpi;
  mbedtls_mpi_init(&serial_mpi);
  result = mbedtls_mpi_read_binary(&serial_mpi, serial, sizeof(serial));
  if (result == 0) {
    result = mbedtls_x509write_crt_set_serial(&certificate, &serial_mpi);
  }
  mbedtls_mpi_free(&serial_mpi);
#endif
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate serial", result));
  }

  constexpr const char* kLocalSendUserName = "CN=LocalSend User";
  result = mbedtls_x509write_crt_set_subject_name(&certificate, kLocalSendUserName);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate subject", result));
  }
  result = mbedtls_x509write_crt_set_issuer_name(&certificate, kLocalSendUserName);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate issuer", result));
  }

  result = mbedtls_x509write_crt_set_validity(&certificate, "20200101000000", "20400101000000");
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate validity", result));
  }

  result = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate constraints", result));
  }
  result = mbedtls_x509write_crt_set_subject_key_identifier(&certificate);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate subject key identifier", result));
  }
  result = mbedtls_x509write_crt_set_authority_key_identifier(&certificate);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate authority key identifier", result));
  }

  std::vector<unsigned char> certificate_buffer(4096);
  result = mbedtls_x509write_crt_pem(&certificate,
                                     certificate_buffer.data(),
                                     certificate_buffer.size(),
                                     mbedtls_ctr_drbg_random,
                                     &ctr_drbg);
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("certificate PEM write", result));
  }

  std::vector<unsigned char> key_buffer(4096);
  result = mbedtls_pk_write_key_pem(&key, key_buffer.data(), key_buffer.size());
  if (result != 0) {
    cleanup();
    throw std::runtime_error(mbedtls_error_text("private key PEM write", result));
  }

  TlsIdentity identity = identity_from_pem(reinterpret_cast<const char*>(certificate_buffer.data()),
                                          reinterpret_cast<const char*>(key_buffer.data()));
  cleanup();
  return identity;
}

} // namespace

std::vector<std::uint8_t> certificate_der_from_pem(const std::string& pem) {
  return base64_decode(pem_body(pem));
}

std::string sha256_hex(const std::vector<std::uint8_t>& data) {
  std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
  std::vector<std::uint8_t> padded = data;
  padded.push_back(0x80);
  while ((padded.size() % 64) != 56) {
    padded.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    padded.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xff));
  }

  std::array<std::uint32_t, 8> hash = {
      0x6a09e667u,
      0xbb67ae85u,
      0x3c6ef372u,
      0xa54ff53au,
      0x510e527fu,
      0x9b05688cu,
      0x1f83d9abu,
      0x5be0cd19u,
  };

  for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
    std::uint32_t words[64] = {};
    for (int i = 0; i < 16; ++i) {
      const std::size_t index = offset + static_cast<std::size_t>(i) * 4;
      words[i] = (static_cast<std::uint32_t>(padded[index]) << 24) |
                 (static_cast<std::uint32_t>(padded[index + 1]) << 16) |
                 (static_cast<std::uint32_t>(padded[index + 2]) << 8) |
                 static_cast<std::uint32_t>(padded[index + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
      const std::uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    std::uint32_t e = hash[4];
    std::uint32_t f = hash[5];
    std::uint32_t g = hash[6];
    std::uint32_t h = hash[7];

    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[i] + words[i];
      const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::array<std::uint8_t, 32> bytes = {};
  for (std::size_t i = 0; i < hash.size(); ++i) {
    bytes[i * 4] = static_cast<std::uint8_t>((hash[i] >> 24) & 0xff);
    bytes[i * 4 + 1] = static_cast<std::uint8_t>((hash[i] >> 16) & 0xff);
    bytes[i * 4 + 2] = static_cast<std::uint8_t>((hash[i] >> 8) & 0xff);
    bytes[i * 4 + 3] = static_cast<std::uint8_t>(hash[i] & 0xff);
  }
  return hex_encode(bytes);
}

std::string certificate_fingerprint_from_pem(const std::string& pem) {
  return sha256_hex(certificate_der_from_pem(pem));
}

TlsIdentity load_or_create_tls_identity(const std::filesystem::path& certificate_path, const std::filesystem::path& private_key_path) {
  if (std::filesystem::exists(certificate_path) && std::filesystem::exists(private_key_path)) {
    return identity_from_pem(read_text_file(certificate_path), read_text_file(private_key_path));
  }

  TlsIdentity identity = generate_tls_identity();
  write_text_file(certificate_path, identity.certificate_pem);
  write_text_file(private_key_path, identity.private_key_pem);
  return identity;
}

} // namespace localsend
