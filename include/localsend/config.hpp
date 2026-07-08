#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace localsend {

enum class PlatformKind {
  Desktop,
  Switch,
  Psv,
};

struct AppConfig {
  std::string alias = "LocalSend Handheld";
  std::filesystem::path inbox_path;
  std::filesystem::path outbox_path;
  std::filesystem::path config_path;
  std::filesystem::path certificate_path;
  std::filesystem::path private_key_path;
  int port = 53317;
  bool discovery_enabled = true;
  bool auto_accept = false;
  struct ManualDevice {
    std::string ip;
    int port = 53317;
    bool https = false;
    std::string alias;
    std::string fingerprint;
  };
  std::vector<ManualDevice> manual_devices;
};

AppConfig default_config(PlatformKind platform);
AppConfig load_config(PlatformKind platform, const std::filesystem::path& path);
void save_config(const AppConfig& config, const std::filesystem::path& path);

} // namespace localsend
