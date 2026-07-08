#pragma once

#include "localsend/config.hpp"
#include "localsend/protocol.hpp"

#include <string>

namespace localsend::handheld {

struct HandheldAppConfig {
  PlatformKind platform = PlatformKind::Desktop;
  std::string alias = "LocalSend Handheld";
  std::string device_model = "Handheld";
  DeviceType device_type = DeviceType::Mobile;
  std::string renderer = "borealis";
  std::string inbox_path;
  std::string outbox_path;
  std::string config_path;
  std::string log_path;
  int port = 53317;
  bool enable_tls = false;
  bool enable_discovery = true;
};

int run_handheld_app(const HandheldAppConfig& config);

} // namespace localsend::handheld
