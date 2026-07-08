#include "localsend/app_service.hpp"

#include "localsend/discovery.hpp"
#include "localsend/security.hpp"

#include <algorithm>
#include <utility>

namespace localsend {
namespace {

std::string default_device_model(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return "Nintendo Switch";
  case PlatformKind::Psv:
    return "PlayStation Vita";
  case PlatformKind::Desktop:
    return "Desktop prototype";
  }
  return "LocalSend Handheld";
}

} // namespace

AppService::AppService(AppConfig config, AppServiceOptions options)
    : config_(std::move(config)), options_(std::move(options)), self_(make_self_info()) {}

AppService::~AppService() {
  wait_for_send_idle();
  stop_discovery();
  stop_server();
}

AppServiceStatus AppService::status() const {
  AppServiceStatus status;
  status.server_running = server_running();
  status.discovery_running = discovery_running();
  status.send_running = send_running();
  status.https = self_.protocol == ProtocolType::Https;
  status.alias = self_.alias;
  status.fingerprint = self_.fingerprint;
  status.port = self_.port;
  status.device_count = devices_.snapshot().size();
  status.transfer_count = transfers_.snapshot().size();
  return status;
}

AppSnapshot AppService::snapshot() const {
  AppSnapshot snapshot;
  snapshot.status = status();
  snapshot.self = self_;
  snapshot.devices = devices_.snapshot();
  snapshot.transfers = transfers_.snapshot();
  return snapshot;
}

bool AppService::start_server() {
  if (server_) {
    return true;
  }

  self_ = make_self_info();
  try {
    if (options_.enable_tls) {
      const TlsIdentity identity = load_or_create_tls_identity(config_.certificate_path, config_.private_key_path);
      self_.protocol = ProtocolType::Https;
      self_.fingerprint = identity.fingerprint;
      server_ = std::make_unique<LocalSendServer>(self_,
                                                  config_.inbox_path,
                                                  TlsCredentials{identity.certificate_pem, identity.private_key_pem},
                                                  &transfers_);
    } else {
      self_.protocol = ProtocolType::Http;
      self_.fingerprint.clear();
      server_ = std::make_unique<LocalSendServer>(self_, config_.inbox_path, &transfers_);
    }
  } catch (const std::exception&) {
    server_.reset();
    return false;
  }

  if (!server_->start(config_.port)) {
    server_.reset();
    return false;
  }
  self_.port = server_->port();
  return true;
}

void AppService::stop_server() {
  if (!server_) {
    return;
  }
  server_->stop();
  server_.reset();
}

bool AppService::announce_once() const {
  if (!config_.discovery_enabled) {
    return false;
  }

  MulticastDto announcement;
  static_cast<InfoRegisterDto&>(announcement) = self_;
  announcement.announce = true;
  return announce_multicast(announcement);
}

int AppService::refresh_discovery(std::chrono::milliseconds timeout) {
  if (!config_.discovery_enabled) {
    return 0;
  }

  int added_or_updated = 0;
  for (auto& device : discover_peers(timeout)) {
    if (is_self_device(device)) {
      continue;
    }
    devices_.upsert_discovered(std::move(device));
    ++added_or_updated;
  }
  return added_or_updated;
}

bool AppService::start_discovery(std::chrono::milliseconds interval, std::chrono::milliseconds scan_timeout) {
  if (discovery_running_) {
    return true;
  }
  if (!config_.discovery_enabled) {
    return false;
  }

  discovery_running_ = true;
  discovery_thread_ = std::thread(&AppService::discovery_loop, this, interval, scan_timeout);
  return true;
}

void AppService::stop_discovery() {
  if (!discovery_running_) {
    return;
  }
  discovery_running_ = false;
  if (discovery_thread_.joinable()) {
    discovery_thread_.join();
  }
}

std::string AppService::add_manual_device(std::string ip,
                                          int port,
                                          bool https,
                                          std::string alias,
                                          std::string fingerprint) {
  Device device;
  device.ip = std::move(ip);
  device.port = port;
  device.https = https;
  device.alias = std::move(alias);
  device.fingerprint = std::move(fingerprint);
  device.version = "2.1";
  return devices_.upsert_manual(std::move(device));
}

bool AppService::send_files_to_device(const std::string& device_key, const std::vector<std::filesystem::path>& file_paths) {
  const std::optional<DeviceEntry> entry = devices_.get(device_key);
  if (!entry) {
    return false;
  }
  return send_files_http(entry->device, file_paths, self_, &transfers_);
}

bool AppService::start_send_to_device(const std::string& device_key, std::vector<std::filesystem::path> file_paths) {
  if (send_running_) {
    return false;
  }

  if (send_thread_.joinable()) {
    send_thread_.join();
  }

  const std::optional<DeviceEntry> entry = devices_.get(device_key);
  if (!entry || file_paths.empty()) {
    return false;
  }

  send_running_ = true;
  send_thread_ = std::thread(&AppService::send_worker, this, entry->device, std::move(file_paths));
  return true;
}

void AppService::wait_for_send_idle() {
  if (send_thread_.joinable()) {
    send_thread_.join();
  }
  send_running_ = false;
}

InfoRegisterDto AppService::make_self_info() const {
  InfoRegisterDto self;
  self.alias = config_.alias;
  self.port = config_.port;
  self.protocol = options_.enable_tls ? ProtocolType::Https : ProtocolType::Http;
  self.fingerprint = "";
  self.device_model = options_.device_model.empty() ? default_device_model(options_.platform) : options_.device_model;
  self.device_type = options_.device_type;
  self.download = false;
  return self;
}

bool AppService::is_self_device(const Device& device) const {
  if (!self_.fingerprint.empty() && device.fingerprint == self_.fingerprint) {
    return true;
  }
  return device.port == self_.port && device.alias == self_.alias;
}

void AppService::discovery_loop(std::chrono::milliseconds interval, std::chrono::milliseconds scan_timeout) {
  const auto bounded_interval = std::max(interval, std::chrono::milliseconds(50));
  const auto bounded_timeout = std::max(scan_timeout, std::chrono::milliseconds(1));

  while (discovery_running_) {
    announce_once();
    refresh_discovery(bounded_timeout);

    const auto sleep_step = std::chrono::milliseconds(50);
    auto slept = std::chrono::milliseconds(0);
    while (discovery_running_ && slept < bounded_interval) {
      const auto remaining = bounded_interval - slept;
      const auto current_sleep = std::min(sleep_step, remaining);
      std::this_thread::sleep_for(current_sleep);
      slept += current_sleep;
    }
  }
}

void AppService::send_worker(Device device, std::vector<std::filesystem::path> file_paths) {
  send_files_http(device, file_paths, self_, &transfers_);
  send_running_ = false;
}

} // namespace localsend
