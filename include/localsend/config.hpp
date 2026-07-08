#pragma once

#include <filesystem>
#include <string>

namespace localsend {

enum class PlatformKind {
  Desktop,
  Switch,
  Psv,
};

struct AppConfig {
  std::string alias = "LocalSend Handheld";
  std::filesystem::path inbox_path;
  std::filesystem::path config_path;
  int port = 53317;
  bool discovery_enabled = true;
  bool auto_accept = false;
};

AppConfig default_config(PlatformKind platform);
AppConfig load_config(PlatformKind platform, const std::filesystem::path& path);
void save_config(const AppConfig& config, const std::filesystem::path& path);

} // namespace localsend

