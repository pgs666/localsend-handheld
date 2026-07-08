#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace localsend {

struct TlsIdentity {
  std::string certificate_pem;
  std::string private_key_pem;
  std::string fingerprint;
};

std::vector<std::uint8_t> certificate_der_from_pem(const std::string& pem);
std::string sha256_hex(const std::vector<std::uint8_t>& data);
std::string certificate_fingerprint_from_pem(const std::string& pem);
TlsIdentity load_or_create_tls_identity(const std::filesystem::path& certificate_path, const std::filesystem::path& private_key_path);

} // namespace localsend
