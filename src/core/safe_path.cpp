#include "localsend/safe_path.hpp"

#include <cctype>

namespace localsend {

std::string sanitize_filename(const std::string& filename) {
  std::string out;
  out.reserve(filename.size());

  for (unsigned char c : filename) {
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
      continue;
    }
    out.push_back(static_cast<char>(c));
  }

  while (out.find("..") != std::string::npos) {
    out.erase(out.find(".."), 2);
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }

  if (out.empty()) {
    return "file";
  }
  return out;
}

std::filesystem::path unique_destination_path(const std::filesystem::path& directory,
                                              const std::string& requested_name) {
  const std::string clean = sanitize_filename(requested_name);
  std::filesystem::path candidate = directory / clean;
  if (!std::filesystem::exists(candidate)) {
    return candidate;
  }

  const std::filesystem::path clean_path(clean);
  const std::string stem = clean_path.stem().string();
  const std::string extension = clean_path.extension().string();

  for (int i = 1; i < 10000; ++i) {
    candidate = directory / (stem + " (" + std::to_string(i) + ")" + extension);
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  return directory / (stem + " (9999)" + extension);
}

} // namespace localsend

