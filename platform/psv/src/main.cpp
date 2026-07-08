#include "localsend/handheld_app.hpp"

#include <cstdio>

extern "C" void flockfile(FILE*) {}
extern "C" void funlockfile(FILE*) {}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  localsend::handheld::HandheldAppConfig config;
  config.platform = localsend::PlatformKind::Psv;
  config.alias = "LocalSend PSV";
  config.device_model = "PlayStation Vita";
  config.device_type = localsend::DeviceType::Mobile;
  config.renderer = "borealis GXM";
  config.inbox_path = "ux0:data/localsend/inbox/";
  config.config_path = "ux0:data/localsend/config.json";
  config.log_path = "ux0:data/localsend-borealis.log";
  config.port = 53317;
  config.enable_tls = false;
  config.enable_discovery = true;
  return localsend::handheld::run_handheld_app(config);
}
