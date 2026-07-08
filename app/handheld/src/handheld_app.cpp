#include "localsend/handheld_app.hpp"

#include "localsend/app_service.hpp"

#include <borealis.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

namespace localsend::handheld {
namespace {

constexpr const char* kProtocol = "LocalSend protocol 2.1";

FILE* g_log = nullptr;

void log_line(const std::string& line) {
  if (!g_log) {
    return;
  }
  std::fprintf(g_log, "%s\n", line.c_str());
  std::fflush(g_log);
}

struct RuntimeState {
  bool server_started = false;
  int server_port = 53317;
  std::string ip = "-";
  std::string server_status = "Not started";
};

brls::Label* make_label(const std::string& text,
                        float size,
                        brls::HorizontalAlign align = brls::HorizontalAlign::LEFT,
                        float width = 780.0f) {
  auto* label = new brls::Label();
  label->setText(text);
  label->setFontSize(size);
  label->setHorizontalAlign(align);
  label->setWidth(width);
  return label;
}

brls::Box* make_row(const std::string& name, const std::string& value) {
  auto* row = new brls::Box(brls::Axis::ROW);
  row->setWidth(780);
  row->setMargins(0, 0, 10, 0);
  row->setAlignItems(brls::AlignItems::CENTER);

  auto* key = make_label(name, 20, brls::HorizontalAlign::LEFT, 230);
  auto* text = make_label(value, 20, brls::HorizontalAlign::LEFT, 520);
  text->setGrow(1.0f);

  row->addView(key);
  row->addView(text);
  return row;
}

brls::Label* make_section(const std::string& text) {
  auto* label = make_label(text, 24, brls::HorizontalAlign::LEFT, 780);
  label->setMargins(26, 0, 14, 0);
  return label;
}

brls::Box* make_panel(const HandheldAppConfig& config, const RuntimeState& state) {
  auto* root = new brls::Box(brls::Axis::COLUMN);
  root->setGrow(1.0f);
  root->setPadding(34, 70, 34, 70);
  root->setAlignItems(brls::AlignItems::CENTER);

  auto* title = make_label("LocalSend Handheld", 38, brls::HorizontalAlign::CENTER, 780);
  title->setMargins(0, 0, 8, 0);
  root->addView(title);

  auto* subtitle = make_label(config.device_model + " UI base", 22, brls::HorizontalAlign::CENTER, 780);
  subtitle->setMargins(0, 0, 18, 0);
  root->addView(subtitle);

  root->addView(make_section("Runtime"));
  root->addView(make_row("Renderer", config.renderer));
  root->addView(make_row("Local IP", state.ip));
  root->addView(make_row("Local port", std::to_string(state.server_port)));
  root->addView(make_row("Protocol", kProtocol));
  root->addView(make_row("Transport", config.enable_tls ? "HTTPS" : "HTTP; Encryption must be off on peer"));

  root->addView(make_section("Paths"));
  root->addView(make_row("Inbox", config.inbox_path));
  root->addView(make_row("Config", config.config_path));
  root->addView(make_row("Log", config.log_path));

  root->addView(make_section("Feature wiring"));
  root->addView(make_row("Receive server", state.server_status));
  root->addView(make_row("Info endpoint",
                         "http://" + state.ip + ":" + std::to_string(state.server_port) +
                             "/api/localsend/v2/info"));
  root->addView(make_row("Discovery", config.enable_discovery ? "Periodic announce enabled" : "Disabled"));
  root->addView(make_row("File browser", "Core exists; controller UI pending"));

  auto* hint = make_label("Press START to exit", 20, brls::HorizontalAlign::CENTER, 780);
  hint->setMargins(24, 0, 0, 0);
  root->addView(hint);

  return root;
}

std::unique_ptr<AppService> start_service(const HandheldAppConfig& app_config, RuntimeState& state) {
  AppConfig config = default_config(app_config.platform);
  config.alias = app_config.alias;
  config.port = app_config.port;
  config.inbox_path = app_config.inbox_path;
  config.config_path = app_config.config_path;
  config.discovery_enabled = app_config.enable_discovery;

  AppServiceOptions options;
  options.platform = app_config.platform;
  options.enable_tls = app_config.enable_tls;
  options.device_model = app_config.device_model;
  options.device_type = app_config.device_type;

  auto service = std::make_unique<AppService>(config, options);
  log_line("Starting receive server on port " + std::to_string(config.port));
  state.server_started = service->start_server();
  if (state.server_started) {
    state.server_port = service->status().port;
    state.server_status = "Started";
    log_line("Receive server started on port " + std::to_string(state.server_port));
    if (service->announce_once()) {
      log_line("Discovery announcement sent");
    } else {
      log_line("Discovery announcement failed");
    }
  } else {
    state.server_status = "Failed to start";
    log_line("Receive server failed to start");
  }
  return service;
}

} // namespace

int run_handheld_app(const HandheldAppConfig& config) {
  try {
    const std::filesystem::path log_path(config.log_path);
    if (log_path.has_parent_path()) {
      std::filesystem::create_directories(log_path.parent_path());
    }
  } catch (const std::exception&) {
  }

  g_log = std::fopen(config.log_path.c_str(), "w+");
  brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
  brls::Logger::setLogOutput(g_log);
  brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;
  log_line("LocalSend Handheld boot");

  if (!brls::Application::init()) {
    log_line("borealis init failed");
    if (g_log) {
      std::fclose(g_log);
      g_log = nullptr;
    }
    return EXIT_FAILURE;
  }

  brls::Application::createWindow("LocalSend Handheld");
  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
  brls::Application::setGlobalQuit(true);

  RuntimeState state;
  state.server_port = config.port;
  state.ip = brls::Application::getPlatform()->getIpAddress();
  log_line("Detected IP: " + state.ip);

  std::unique_ptr<AppService> service;
  try {
    service = start_service(config, state);
  } catch (const std::exception& e) {
    state.server_status = std::string("Exception: ") + e.what();
    log_line(state.server_status);
  } catch (...) {
    state.server_status = "Unknown exception";
    log_line(state.server_status);
  }

  auto* frame = new brls::AppletFrame(make_panel(config, state));
  frame->setTitle("LocalSend Handheld");
  brls::Application::pushActivity(new brls::Activity(frame));

  auto next_announce = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (brls::Application::mainLoop()) {
    if (service && state.server_started && config.enable_discovery &&
        std::chrono::steady_clock::now() >= next_announce) {
      service->announce_once();
      next_announce = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
  }

  service.reset();
  if (g_log) {
    std::fclose(g_log);
    g_log = nullptr;
  }
  return EXIT_SUCCESS;
}

} // namespace localsend::handheld
