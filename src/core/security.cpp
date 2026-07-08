#include "localsend/security.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>

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

} // namespace localsend
