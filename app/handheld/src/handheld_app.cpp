#include "localsend/handheld_app.hpp"

#include "localsend/app_service.hpp"
#include "localsend/file_browser.hpp"
#include "localsend/status_format.hpp"

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
constexpr const char* kUiBuild = "shared-status-refresh-applet-frame";

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
  bool discovery_started = false;
  int server_port = 53317;
  std::string ip = "-";
  std::string server_status = "Not started";
  std::string discovery_status = "Not started";
  std::string file_browser_status = "Not checked";
};

struct PanelRefs {
  brls::Label* server_status = nullptr;
  brls::Label* discovery_status = nullptr;
  brls::Label* device_summary = nullptr;
  brls::Label* transfer_summary = nullptr;
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

brls::Box* make_row(const std::string& name, const std::string& value, brls::Label** value_ref = nullptr) {
  auto* row = new brls::Box(brls::Axis::ROW);
  row->setWidth(780);
  row->setMargins(0, 0, 10, 0);
  row->setAlignItems(brls::AlignItems::CENTER);

  auto* key = make_label(name, 20, brls::HorizontalAlign::LEFT, 230);
  auto* text = make_label(value, 20, brls::HorizontalAlign::LEFT, 520);
  text->setGrow(1.0f);
  if (value_ref) {
    *value_ref = text;
  }

  row->addView(key);
  row->addView(text);
  return row;
}

brls::Label* make_section(const std::string& text) {
  auto* label = make_label(text, 24, brls::HorizontalAlign::LEFT, 780);
  label->setMargins(26, 0, 14, 0);
  return label;
}

brls::View* make_panel(const HandheldAppConfig& config, const RuntimeState& state, PanelRefs& refs) {
  const std::string scheme = config.enable_tls ? "https" : "http";

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
  root->addView(make_row("UI build", kUiBuild));
  root->addView(make_row("Local IP", state.ip));
  root->addView(make_row("Local port", std::to_string(state.server_port)));
  root->addView(make_row("Protocol", kProtocol));
  root->addView(make_row("Transport", config.enable_tls ? "HTTPS" : "HTTP; Encryption must be off on peer"));

  root->addView(make_section("Paths"));
  root->addView(make_row("Inbox", config.inbox_path));
  root->addView(make_row("Outbox", config.outbox_path));
  root->addView(make_row("Config", config.config_path));
  root->addView(make_row("Log", config.log_path));

  root->addView(make_section("Feature wiring"));
  root->addView(make_row("Receive server", state.server_status, &refs.server_status));
  root->addView(make_row("Info endpoint",
                         scheme + "://" + state.ip + ":" + std::to_string(state.server_port) +
                             "/api/localsend/v2/info"));
  root->addView(make_row("Discovery", state.discovery_status, &refs.discovery_status));
  root->addView(make_row("File browser", state.file_browser_status));

  root->addView(make_section("Peers"));
  root->addView(make_row("Known devices", "No peers yet", &refs.device_summary));

  root->addView(make_section("Transfers"));
  root->addView(make_row("Recent", "No transfers yet", &refs.transfer_summary));

  const std::string hint_text =
      config.platform == PlatformKind::Switch
          ? "Receive server starts automatically"
          : "Receive server starts automatically";
  auto* hint = make_label(hint_text, 20, brls::HorizontalAlign::CENTER, 780);
  hint->setMargins(24, 0, 0, 0);
  root->addView(hint);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setContentView(root);
  return scroll;
}

std::string format_outbox_status(const OutboxStatus& status) {
  if (!status.error.empty()) {
    return "Outbox error: " + status.error;
  }
  if (!status.directory_ready) {
    return "Outbox unavailable";
  }
  std::string text = std::to_string(status.selectable_count) + " selectable file";
  if (status.selectable_count != 1) {
    text += "s";
  }
  if (status.sample_ready) {
    text += "; test file ready";
  }
  return text;
}

void refresh_panel(const RuntimeState& state, const PanelRefs& refs) {
  if (refs.server_status) {
    refs.server_status->setText(state.server_status);
  }
  if (refs.discovery_status && !state.server_started) {
    refs.discovery_status->setText("Server not started");
  }
  if (refs.discovery_status && state.server_started) {
    refs.discovery_status->setText(state.discovery_status);
  }
}

void refresh_discovery_status(const std::string& text, const PanelRefs& refs) {
  if (refs.discovery_status) {
    refs.discovery_status->setText(text);
  }
}

void refresh_service_snapshot(const AppService& service, const PanelRefs& refs) {
  const AppSnapshot snapshot = service.snapshot();
  if (refs.device_summary) {
    refs.device_summary->setText(format_device_summary(snapshot.devices));
  }
  if (refs.transfer_summary) {
    refs.transfer_summary->setText(format_transfer_summary(snapshot.transfers));
  }
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
    if (app_config.enable_discovery) {
      const bool announced = service->announce_once();
      log_line(announced ? "Discovery announcement sent" : "Discovery announcement failed");
      state.discovery_started = service->start_discovery();
      if (state.discovery_started) {
        state.discovery_status = "Scanning and announcing";
        log_line("Discovery scan loop started");
      } else {
        state.discovery_status = announced ? "Announce-only fallback" : "Failed";
        log_line("Discovery scan loop failed");
      }
    } else {
      state.discovery_status = "Disabled";
    }
  } else {
    state.server_status = "Failed to start";
    state.discovery_status = "Server not started";
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
  log_line(std::string("UI build: ") + kUiBuild);

  log_line("Calling borealis init");
  if (!brls::Application::init()) {
    log_line("borealis init failed");
    if (g_log) {
      std::fclose(g_log);
      g_log = nullptr;
    }
    return EXIT_FAILURE;
  }
  log_line("borealis init ok");

  log_line("Creating borealis window");
  brls::Application::createWindow("LocalSend Handheld");
  log_line("borealis window created");
  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
  brls::Application::setGlobalQuit(false);

  RuntimeState state;
  state.server_port = config.port;
  state.ip = brls::Application::getPlatform()->getIpAddress();
  log_line("Detected IP: " + state.ip);
  const OutboxStatus outbox_status = prepare_outbox(config.outbox_path, true);
  state.file_browser_status = format_outbox_status(outbox_status);
  log_line("Outbox status: " + state.file_browser_status);

  std::unique_ptr<AppService> service;
  const bool start_service_before_ui = config.platform != PlatformKind::Switch;
  if (start_service_before_ui) {
    try {
      service = start_service(config, state);
    } catch (const std::exception& e) {
      state.server_status = std::string("Exception: ") + e.what();
      log_line(state.server_status);
    } catch (...) {
      state.server_status = "Unknown exception";
      log_line(state.server_status);
    }
  } else {
    state.server_status = "Waiting for UI";
    log_line("Switch startup: receive server deferred until first stable frames");
  }

  PanelRefs refs;
  auto* panel = make_panel(config, state, refs);
  refresh_panel(state, refs);

  auto* frame = new brls::AppletFrame(panel);
  frame->setTitle("LocalSend Handheld");
  log_line("Using AppletFrame bottom-bar shell");
  log_line("Pushing activity");
  brls::Application::pushActivity(new brls::Activity(frame));
  log_line("Activity pushed; entering main loop");

  auto next_announce = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto next_snapshot_refresh = std::chrono::steady_clock::now();
  auto deferred_start = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  int loop_count = 0;
  while (brls::Application::mainLoop()) {
    if (++loop_count == 1) {
      log_line("mainLoop first frame ok");
    } else if (loop_count % 600 == 0) {
      log_line("mainLoop alive");
    }

    if (!service && !state.server_started && config.platform == PlatformKind::Switch &&
        std::chrono::steady_clock::now() >= deferred_start) {
      log_line("Deferred Switch receive server start");
      try {
        service = start_service(config, state);
      } catch (const std::exception& e) {
        state.server_status = std::string("Exception: ") + e.what();
        log_line(state.server_status);
      } catch (...) {
        state.server_status = "Unknown exception";
        log_line(state.server_status);
      }
      refresh_panel(state, refs);
      next_announce = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }

    if (service && state.server_started && config.platform == PlatformKind::Switch) {
      service->poll_server_once();
    }

    if (service && state.server_started && config.enable_discovery && !state.discovery_started &&
        std::chrono::steady_clock::now() >= next_announce) {
      const bool ok = service->announce_once();
      refresh_discovery_status(ok ? "Periodic announce sent" : "Periodic announce failed", refs);
      next_announce = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    if (service && std::chrono::steady_clock::now() >= next_snapshot_refresh) {
      refresh_service_snapshot(*service, refs);
      next_snapshot_refresh = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
  }
  log_line("mainLoop exited");

  service.reset();
  if (g_log) {
    std::fclose(g_log);
    g_log = nullptr;
  }
  return EXIT_SUCCESS;
}

} // namespace localsend::handheld
