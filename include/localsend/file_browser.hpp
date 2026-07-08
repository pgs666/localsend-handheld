#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace localsend {

struct FileEntry {
  std::filesystem::path path;
  std::string name;
  bool directory = false;
  std::uint64_t size = 0;
};

struct DirectoryListing {
  std::filesystem::path path;
  std::filesystem::path parent;
  bool has_parent = false;
  std::vector<FileEntry> entries;
};

DirectoryListing list_directory(const std::filesystem::path& path);
std::vector<std::filesystem::path> selectable_files(const DirectoryListing& listing);

} // namespace localsend
