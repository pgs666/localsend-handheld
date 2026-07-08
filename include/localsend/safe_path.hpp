#pragma once

#include <filesystem>
#include <string>

namespace localsend {

std::string sanitize_filename(const std::string& filename);
std::filesystem::path unique_destination_path(const std::filesystem::path& directory,
                                              const std::string& requested_name);

} // namespace localsend

