#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace localsend {

std::vector<std::uint8_t> certificate_der_from_pem(const std::string& pem);
std::string sha256_hex(const std::vector<std::uint8_t>& data);
std::string certificate_fingerprint_from_pem(const std::string& pem);

} // namespace localsend
