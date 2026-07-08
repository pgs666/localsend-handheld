#include "localsend/handheld_app.hpp"

#include "localsend/app_service.hpp"
#include "localsend/device_selection.hpp"
#include "localsend/file_browser.hpp"
#include "localsend/status_format.hpp"

#include <borealis.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

struct KeyLabels {
  std::string send;
  std::string send_all;
  std::string cancel;
  std::string next_peer;
  std::string next_file;
  std::string restart_core;
  std::string save_peer;
  std::string remove_peer;
};

KeyLabels key_labels_for_platform(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return {"X", "Plus", "B", "Y", "R", "A", "L", "Minus"};
  case PlatformKind::Psv:
    return {"Square", "Start", "Circle", "Triangle", "R", "Cross", "L", "Select"};
  case PlatformKind::Desktop:
    return {"X", "Start", "B", "Y", "RB", "A", "LB", "Back"};
  }
  return {"X", "Start", "B", "Y", "RB", "A", "LB", "Back"};
}

std::string initial_send_prompt(const KeyLabels& keys) {
  return "Select peer/file, then press " + keys.send;
}

std::string action_label(const std::string& action, const std::string& key) {
  return action + " [" + key + "]";
}

struct RuntimeState {
  bool server_started = false;
  bool discovery_started = false;
  int server_port = 53317;
  std::string ip = "-";
  std::string alias = "LocalSend Handheld";
  std::string server_status = "Not started";
  std::string discovery_status = "Not started";
  std::string file_browser_status = "Not checked";
  std::string selected_file_status = "No selectable files";
  std::string send_status = "Select peer/file, then press X";
  std::string last_send_error;
  std::string last_send_status_message;
  std::size_t selected_file_index = 0;
  std::optional<std::size_t> selected_device_index;
  std::string selected_peer_status = "No online peer selected";
};

struct PanelRefs {
  brls::Label* server_status = nullptr;
  brls::Label* discovery_status = nullptr;
  brls::Label* selected_file = nullptr;
  brls::Label* send_status = nullptr;
  brls::Label* selected_peer = nullptr;
  brls::Label* device_summary = nullptr;
  brls::Label* manual_peers = nullptr;
  brls::Label* transfer_summary = nullptr;
};

AppConfig platform_default_config(const HandheldAppConfig& app_config) {
  AppConfig config = default_config(app_config.platform);
  config.alias = app_config.alias;
  config.port = app_config.port;
  config.inbox_path = app_config.inbox_path;
  config.outbox_path = app_config.outbox_path;
  config.config_path = app_config.config_path;
  config.discovery_enabled = app_config.enable_discovery;
  return config;
}

AppConfig load_handheld_config(const HandheldAppConfig& app_config) {
  AppConfig config = platform_default_config(app_config);
  try {
    if (std::filesystem::exists(config.config_path)) {
      config = load_config(app_config.platform, config.config_path);
      config.config_path = app_config.config_path;
      log_line("Loaded config: " + config.config_path.string());
    } else {
      save_config(config, config.config_path);
      log_line("Created default config: " + config.config_path.string());
    }
  } catch (const std::exception& e) {
    log_line(std::string("Config load failed; using defaults: ") + e.what());
  }

  config.inbox_path = app_config.inbox_path;
  config.outbox_path = app_config.outbox_path;
  config.config_path = app_config.config_path;
  return config;
}

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

brls::View* make_panel(const HandheldAppConfig& app_config,
                       const AppConfig& service_config,
                       const RuntimeState& state,
                       PanelRefs& refs) {
  const std::string scheme = app_config.enable_tls ? "https" : "http";

  auto* root = new brls::Box(brls::Axis::COLUMN);
  root->setGrow(1.0f);
  root->setPadding(34, 70, 34, 70);
  root->setAlignItems(brls::AlignItems::CENTER);

  auto* title = make_label("LocalSend Handheld", 38, brls::HorizontalAlign::CENTER, 780);
  title->setMargins(0, 0, 8, 0);
  root->addView(title);

  auto* subtitle = make_label(app_config.device_model + " UI base", 22, brls::HorizontalAlign::CENTER, 780);
  subtitle->setMargins(0, 0, 18, 0);
  root->addView(subtitle);

  root->addView(make_section("Runtime"));
  root->addView(make_row("Renderer", app_config.renderer));
  root->addView(make_row("UI build", kUiBuild));
  root->addView(make_row("Alias", state.alias));
  root->addView(make_row("Local IP", state.ip));
  root->addView(make_row("Local port", std::to_string(state.server_port)));
  root->addView(make_row("Protocol", kProtocol));
  root->addView(make_row("Transport", app_config.enable_tls ? "HTTPS" : "HTTP; Encryption must be off on peer"));

  root->addView(make_section("Paths"));
  root->addView(make_row("Inbox", service_config.inbox_path.string()));
  root->addView(make_row("Outbox", service_config.outbox_path.string()));
  root->addView(make_row("Config", service_config.config_path.string()));
  root->addView(make_row("Log", app_config.log_path));

  root->addView(make_section("Feature wiring"));
  root->addView(make_row("Receive server", state.server_status, &refs.server_status));
  root->addView(make_row("Info endpoint",
                         scheme + "://" + state.ip + ":" + std::to_string(state.server_port) +
                             "/api/localsend/v2/info"));
  root->addView(make_row("Discovery", state.discovery_status, &refs.discovery_status));
  root->addView(make_row("File browser", state.file_browser_status));
  root->addView(make_row("Selected file", state.selected_file_status, &refs.selected_file));
  root->addView(make_row("Send action", state.send_status, &refs.send_status));

  root->addView(make_section("Peers"));
  root->addView(make_row("Selected", state.selected_peer_status, &refs.selected_peer));
  root->addView(make_row("Known devices", "No peers yet", &refs.device_summary));
  root->addView(make_row("Manual peers", std::to_string(service_config.manual_devices.size()), &refs.manual_peers));

  root->addView(make_section("Transfers"));
  root->addView(make_row("Recent", "No transfers yet", &refs.transfer_summary));

  const KeyLabels keys = key_labels_for_platform(app_config.platform);
  const std::string hint_text =
      keys.send + " send  " + keys.send_all + " all  " + keys.next_peer + " peer  " + keys.next_file + " file  " +
      keys.save_peer + " save  " + keys.remove_peer + " remove  " + keys.cancel + " cancel";
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

void refresh_send_status(const std::string& text, const PanelRefs& refs) {
  if (refs.send_status) {
    refs.send_status->setText(text);
  }
}

void refresh_selected_file_status(const std::filesystem::path& outbox_path, RuntimeState& state, const PanelRefs& refs) {
  state.selected_file_status = format_file_choice(outbox_path, state.selected_file_index);
  if (refs.selected_file) {
    refs.selected_file->setText(state.selected_file_status);
  }
}

std::string format_send_target_log(const DeviceEntry& selected) {
  const Device& device = selected.device;
  std::string text = selected.key;
  text += " alias=" + (device.alias.empty() ? std::string("<empty>") : device.alias);
  text += " ip=" + device.ip + ":" + std::to_string(device.port);
  text += " protocol=" + std::string(device.https ? "https" : "http");
  text += " fingerprint=" + (device.fingerprint.empty() ? std::string("<empty>") : device.fingerprint);
  text += " source=" + std::string(to_string(selected.source));
  return text;
}

void refresh_service_snapshot(AppService& service, RuntimeState& state, const PanelRefs& refs) {
  const AppSnapshot snapshot = service.snapshot();
  if (!selected_online_device(snapshot.devices, state.selected_device_index)) {
    state.selected_device_index = first_online_device_index(snapshot.devices);
  }
  state.selected_peer_status = format_selected_device(snapshot.devices, state.selected_device_index);
  if (refs.selected_peer) {
    refs.selected_peer->setText(state.selected_peer_status);
  }
  if (refs.device_summary) {
    refs.device_summary->setText(format_device_summary(snapshot.devices, 5));
  }
  if (refs.manual_peers) {
    refs.manual_peers->setText(std::to_string(service.config().manual_devices.size()));
  }
  if (refs.transfer_summary) {
    refs.transfer_summary->setText(format_transfer_summary(snapshot.transfers));
  }
  if (!snapshot.status.last_send_error.empty()) {
    if (snapshot.status.last_send_error != state.last_send_error) {
      log_line("Send failed: " + snapshot.status.last_send_error);
    }
    state.send_status = "Send failed: " + snapshot.status.last_send_error;
    refresh_send_status(state.send_status, refs);
  } else if (snapshot.status.send_running) {
    if (!snapshot.status.send_status_message.empty() &&
        snapshot.status.send_status_message != state.last_send_status_message) {
      log_line("Send progress: " + snapshot.status.send_status_message);
    }
    state.send_status = snapshot.status.send_status_message.empty() ? "Sending to selected peer" : snapshot.status.send_status_message;
    refresh_send_status(state.send_status, refs);
  } else if (!snapshot.status.send_status_message.empty()) {
    if (snapshot.status.send_status_message != state.last_send_status_message) {
      log_line("Send progress: " + snapshot.status.send_status_message);
    }
    state.send_status = snapshot.status.send_status_message;
    refresh_send_status(state.send_status, refs);
  }
  state.last_send_error = snapshot.status.last_send_error;
  state.last_send_status_message = snapshot.status.send_status_message;
}

std::unique_ptr<AppService> start_service(const HandheldAppConfig& app_config,
                                          const AppConfig& config,
                                          RuntimeState& state) {
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
    if (config.discovery_enabled) {
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
    const std::string error = service->last_server_error();
    log_line("Receive server failed to start" + (error.empty() ? std::string() : ": " + error));
  }
  return service;
}

void update_core_state_from_service(AppService& service, RuntimeState& state) {
  const AppServiceStatus status = service.status();
  state.server_started = status.server_running;
  state.discovery_started = status.discovery_running;
  state.server_port = status.port == 0 ? state.server_port : status.port;
  state.server_status = status.server_running ? "Started" : "Failed to start";
  if (status.discovery_running) {
    state.discovery_status = "Scanning and announcing";
  } else if (service.config().discovery_enabled && status.server_running) {
    state.discovery_status = "Announce-only fallback";
  } else if (service.config().discovery_enabled) {
    state.discovery_status = "Server not started";
  } else {
    state.discovery_status = "Disabled";
  }
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

  const AppConfig service_config = load_handheld_config(config);
  const KeyLabels keys = key_labels_for_platform(config.platform);
  RuntimeState state;
  state.server_port = service_config.port;
  state.alias = service_config.alias;
  state.send_status = initial_send_prompt(keys);
  state.ip = brls::Application::getPlatform()->getIpAddress();
  log_line("Detected IP: " + state.ip);
  log_line("Runtime alias: " + state.alias);
  log_line("Manual peers configured: " + std::to_string(service_config.manual_devices.size()));
  const OutboxStatus outbox_status = prepare_outbox(service_config.outbox_path, true);
  state.file_browser_status = format_outbox_status(outbox_status);
  state.selected_file_status = format_file_choice(service_config.outbox_path, state.selected_file_index);
  log_line("Outbox status: " + state.file_browser_status);
  log_line("Selected file: " + state.selected_file_status);

  std::unique_ptr<AppService> service;
  const bool start_service_before_ui = config.platform == PlatformKind::Desktop;
  if (start_service_before_ui) {
    try {
      service = start_service(config, service_config, state);
    } catch (const std::exception& e) {
      state.server_status = std::string("Exception: ") + e.what();
      log_line(state.server_status);
    } catch (...) {
      state.server_status = "Unknown exception";
      log_line(state.server_status);
    }
  } else {
    state.server_status = "Waiting for UI";
    log_line("Handheld startup: receive server deferred until first stable frames");
  }

  PanelRefs refs;
  auto* panel = make_panel(config, service_config, state, refs);
  refresh_panel(state, refs);

  auto* frame = new brls::AppletFrame(panel);
  frame->setTitle("LocalSend Handheld");
  frame->registerAction(action_label("Send", keys.send), brls::BUTTON_X, [&](brls::View*) {
    if (!service || !state.server_started) {
      state.send_status = "Receive server not ready";
      refresh_send_status(state.send_status, refs);
      log_line("Send action rejected: service not ready");
      return true;
    }

    const AppSnapshot snapshot = service->snapshot();
    const auto selected = selected_online_device(snapshot.devices, state.selected_device_index);
    if (!selected) {
      state.send_status = "No online peer selected";
      refresh_send_status(state.send_status, refs);
      log_line("Send action rejected: no online peer");
      return true;
    }

    const std::optional<std::filesystem::path> file = selectable_file_at(service_config.outbox_path, state.selected_file_index);
    if (!file) {
      state.send_status = "Outbox empty";
      refresh_send_status(state.send_status, refs);
      log_line("Send action rejected: outbox empty");
      return true;
    }

    if (service->start_send_to_device(selected->key, std::vector<std::filesystem::path>{*file})) {
      const std::string peer = selected->device.alias.empty() ? selected->device.ip : selected->device.alias;
      state.send_status = "Sending " + file->filename().string() + " to " + peer;
      log_line("Send action started file=" + file->string() + " target=" + format_send_target_log(*selected));
    } else {
      const std::string error = service->last_send_error();
      state.send_status = error.empty() ? "Send start failed" : "Send failed: " + error;
      log_line("Send action failed to start: " + state.send_status);
    }
    refresh_send_status(state.send_status, refs);
    return true;
  });
  frame->registerAction(action_label("Send all", keys.send_all), brls::BUTTON_START, [&](brls::View*) {
    if (!service || !state.server_started) {
      state.send_status = "Receive server not ready";
      refresh_send_status(state.send_status, refs);
      log_line("Send all rejected: service not ready");
      return true;
    }

    const AppSnapshot snapshot = service->snapshot();
    const auto selected = selected_online_device(snapshot.devices, state.selected_device_index);
    if (!selected) {
      state.send_status = "No online peer selected";
      refresh_send_status(state.send_status, refs);
      log_line("Send all rejected: no online peer");
      return true;
    }

    const std::vector<std::filesystem::path> files = selectable_files(list_directory(service_config.outbox_path));
    if (files.empty()) {
      state.send_status = "Outbox empty";
      refresh_send_status(state.send_status, refs);
      log_line("Send all rejected: outbox empty");
      return true;
    }

    if (service->start_send_to_device(selected->key, files)) {
      const std::string peer = selected->device.alias.empty() ? selected->device.ip : selected->device.alias;
      state.send_status = "Sending " + std::to_string(files.size()) + " files to " + peer;
      log_line("Send all started count=" + std::to_string(files.size()) + " target=" + format_send_target_log(*selected));
    } else {
      const std::string error = service->last_send_error();
      state.send_status = error.empty() ? "Send all failed" : "Send failed: " + error;
      log_line("Send all failed to start: " + state.send_status);
    }
    refresh_send_status(state.send_status, refs);
    return true;
  });
  frame->registerAction(action_label("Cancel send", keys.cancel), brls::BUTTON_B, [&](brls::View*) {
    if (!service) {
      log_line("Back pressed while service not ready; opening exit prompt");
      frame->popContentView();
      return true;
    }

    const AppSnapshot snapshot = service->snapshot();
    if (!snapshot.status.send_running) {
      log_line("Back pressed while idle; opening exit prompt");
      frame->popContentView();
      return true;
    }

    if (service->cancel_current_send()) {
      state.send_status = "Cancelling send...";
      log_line("Cancel send requested");
    } else {
      const std::string error = service->last_send_error();
      state.send_status = error.empty() ? "No active send" : error;
      log_line("Cancel send rejected: " + state.send_status);
    }
    refresh_send_status(state.send_status, refs);
    return true;
  });
  frame->registerAction(action_label("Next peer", keys.next_peer), brls::BUTTON_Y, [&](brls::View*) {
    if (!service) {
      state.selected_peer_status = "Receive server not ready";
      if (refs.selected_peer) {
        refs.selected_peer->setText(state.selected_peer_status);
      }
      return true;
    }

    const AppSnapshot snapshot = service->snapshot();
    state.selected_device_index = next_online_device_index(snapshot.devices, state.selected_device_index);
    state.selected_peer_status = format_selected_device(snapshot.devices, state.selected_device_index);
    if (refs.selected_peer) {
      refs.selected_peer->setText(state.selected_peer_status);
    }
    log_line("Selected peer: " + state.selected_peer_status);
    return true;
  });
  frame->registerAction(action_label("Next file", keys.next_file), brls::BUTTON_RB, [&](brls::View*) {
    ++state.selected_file_index;
    refresh_selected_file_status(service_config.outbox_path, state, refs);
    log_line("Selected file: " + state.selected_file_status);
    return true;
  });
  frame->registerAction(action_label("Restart core", keys.restart_core), brls::BUTTON_A, [&](brls::View*) {
    if (!service) {
      state.server_status = "Restarting";
      refresh_panel(state, refs);
      log_line("Core restart requested before service exists");
      try {
        service = start_service(config, service_config, state);
      } catch (const std::exception& e) {
        state.server_status = std::string("Exception: ") + e.what();
        log_line(state.server_status);
      } catch (...) {
        state.server_status = "Unknown exception";
        log_line(state.server_status);
      }
      refresh_panel(state, refs);
      return true;
    }

    log_line("Core restart requested");
    state.server_status = "Restarting";
    state.discovery_status = "Restarting";
    refresh_panel(state, refs);
    const bool ok = service->restart_core();
    update_core_state_from_service(*service, state);
    if (!ok) {
      const std::string error = service->last_server_error();
      log_line("Core restart failed" + (error.empty() ? std::string() : ": " + error));
    } else {
      log_line("Core restart ok");
    }
    refresh_panel(state, refs);
    return true;
  });
  frame->registerAction(action_label("Save peer", keys.save_peer), brls::BUTTON_LB, [&](brls::View*) {
    if (!service) {
      state.send_status = "Receive server not ready";
      refresh_send_status(state.send_status, refs);
      log_line("Save peer rejected: service not ready");
      return true;
    }

    const AppSnapshot snapshot = service->snapshot();
    const auto selected = selected_online_device(snapshot.devices, state.selected_device_index);
    if (!selected) {
      state.send_status = "No online peer selected";
      refresh_send_status(state.send_status, refs);
      log_line("Save peer rejected: no online peer");
      return true;
    }

    const Device& device = selected->device;
    const std::string key = service->add_manual_device(device.ip,
                                                       device.port,
                                                       device.https,
                                                       device.alias,
                                                       device.fingerprint);
    if (service->save_config()) {
      state.send_status = "Saved peer: " + (device.alias.empty() ? device.ip : device.alias);
      log_line("Saved peer target=" + format_send_target_log(*selected) + " key=" + key);
    } else {
      state.send_status = "Save peer failed";
      log_line("Save peer failed target=" + format_send_target_log(*selected) + " key=" + key);
    }
    refresh_send_status(state.send_status, refs);
    refresh_service_snapshot(*service, state, refs);
    return true;
  });
  frame->registerAction(action_label("Remove peer", keys.remove_peer), brls::BUTTON_BACK, [&](brls::View*) {
    if (!service) {
      state.send_status = "Receive server not ready";
      refresh_send_status(state.send_status, refs);
      log_line("Remove peer rejected: service not ready");
      return true;
    }

    const AppSnapshot snapshot = service->snapshot();
    const auto selected = selected_online_device(snapshot.devices, state.selected_device_index);
    if (!selected) {
      state.send_status = "No online peer selected";
      refresh_send_status(state.send_status, refs);
      log_line("Remove peer rejected: no online peer");
      return true;
    }
    if (selected->source != DeviceSource::Manual) {
      state.send_status = "Selected peer is not manual";
      refresh_send_status(state.send_status, refs);
      log_line("Remove peer rejected: selected peer is not manual target=" + format_send_target_log(*selected));
      return true;
    }

    const std::string peer = selected->device.alias.empty() ? selected->device.ip : selected->device.alias;
    if (!service->remove_manual_device(selected->key)) {
      state.send_status = "Remove peer failed";
      log_line("Remove peer failed target=" + format_send_target_log(*selected));
      refresh_send_status(state.send_status, refs);
      return true;
    }

    if (service->save_config()) {
      state.send_status = "Removed peer: " + peer;
      log_line("Removed peer target=" + format_send_target_log(*selected));
    } else {
      state.send_status = "Removed peer; save failed";
      log_line("Removed peer but save failed target=" + format_send_target_log(*selected));
    }
    state.selected_device_index = std::nullopt;
    refresh_send_status(state.send_status, refs);
    refresh_service_snapshot(*service, state, refs);
    return true;
  });
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

    if (!service && !state.server_started && config.platform != PlatformKind::Desktop &&
        std::chrono::steady_clock::now() >= deferred_start) {
      log_line("Deferred handheld receive server start");
      try {
        service = start_service(config, service_config, state);
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

    if (service && state.server_started && service_config.discovery_enabled && !state.discovery_started &&
        std::chrono::steady_clock::now() >= next_announce) {
      const bool ok = service->announce_once();
      refresh_discovery_status(ok ? "Periodic announce sent" : "Periodic announce failed", refs);
      next_announce = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    if (service && std::chrono::steady_clock::now() >= next_snapshot_refresh) {
      refresh_service_snapshot(*service, state, refs);
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
