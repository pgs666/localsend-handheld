#include "localsend/handheld_app.hpp"

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  localsend::handheld::HandheldAppConfig config;
  config.platform = localsend::PlatformKind::Switch;
  config.alias = "LocalSend Switch";
  config.device_model = "Nintendo Switch";
  config.device_type = localsend::DeviceType::Desktop;
  config.renderer = "borealis Switch";
  config.inbox_path = "sdmc:/switch/localsend/inbox/";
  config.outbox_path = "sdmc:/switch/localsend/outbox/";
  config.config_path = "sdmc:/switch/localsend/config.json";
  config.log_path = "sdmc:/switch/localsend/localsend-borealis.log";
  config.port = 53317;
  config.enable_tls = true;
  config.enable_discovery = true;
  return localsend::handheld::run_handheld_app(config);
}
