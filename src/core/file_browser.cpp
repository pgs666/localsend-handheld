#include "localsend/file_browser.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace localsend {
namespace {

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool entry_less(const FileEntry& lhs, const FileEntry& rhs) {
  if (lhs.directory != rhs.directory) {
    return lhs.directory;
  }
  return lower_ascii(lhs.name) < lower_ascii(rhs.name);
}

} // namespace

DirectoryListing list_directory(const std::filesystem::path& path) {
  DirectoryListing listing;
  std::error_code ec;
  listing.path = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    listing.path = path;
  }

  const auto parent = listing.path.parent_path();
  listing.has_parent = !parent.empty() && parent != listing.path;
  listing.parent = parent;

  std::filesystem::directory_options options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& item : std::filesystem::directory_iterator(path, options, ec)) {
    if (ec) {
      break;
    }

    FileEntry entry;
    entry.path = item.path();
    entry.name = item.path().filename().string();
    if (entry.name.empty() || entry.name == "." || entry.name == "..") {
      continue;
    }

    entry.directory = item.is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }

    if (!entry.directory && item.is_regular_file(ec)) {
      entry.size = static_cast<std::uint64_t>(item.file_size(ec));
      if (ec) {
        entry.size = 0;
        ec.clear();
      }
    } else {
      ec.clear();
    }

    listing.entries.push_back(std::move(entry));
  }

  std::sort(listing.entries.begin(), listing.entries.end(), entry_less);
  return listing;
}

std::vector<std::filesystem::path> selectable_files(const DirectoryListing& listing) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : listing.entries) {
    if (!entry.directory) {
      files.push_back(entry.path);
    }
  }
  return files;
}

std::optional<std::filesystem::path> first_selectable_file(const std::filesystem::path& path) {
  const auto files = selectable_files(list_directory(path));
  if (files.empty()) {
    return std::nullopt;
  }
  return files.front();
}

OutboxStatus prepare_outbox(const std::filesystem::path& path, bool create_sample_file) {
  OutboxStatus status;
  status.path = path;
  status.sample_file = path / "localsend-handheld-test.txt";

  try {
    std::filesystem::create_directories(path);
    status.directory_ready = std::filesystem::is_directory(path);

    if (create_sample_file && !std::filesystem::exists(status.sample_file)) {
      std::ofstream out(status.sample_file, std::ios::binary);
      out << "LocalSend Handheld test file\n";
    }
    status.sample_ready = std::filesystem::is_regular_file(status.sample_file);

    const auto listing = list_directory(path);
    status.selectable_count = selectable_files(listing).size();
  } catch (const std::exception& e) {
    status.error = e.what();
  }

  return status;
}

} // namespace localsend
