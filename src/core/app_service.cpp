#include "localsend/app_service.hpp"

#include "localsend/discovery.hpp"
#include "localsend/security.hpp"

#include <algorithm>
#if LOCALSEND_PLATFORM_PSV
#include <pthread.h>
#include <unistd.h>
#else
#include <thread>
#endif
#include <utility>

namespace localsend {
namespace {

constexpr auto kDiscoveryStaleAge = std::chrono::seconds(30);

bool same_manual_device(const AppConfig::ManualDevice& lhs, const AppConfig::ManualDevice& rhs) {
  if (!lhs.fingerprint.empty() && !rhs.fingerprint.empty()) {
    return lhs.fingerprint == rhs.fingerprint;
  }
  return lhs.ip == rhs.ip && lhs.port == rhs.port;
}

Device device_from_manual(const AppConfig::ManualDevice& manual) {
  Device device;
  device.ip = manual.ip;
  device.port = manual.port;
  device.https = manual.https;
  device.alias = manual.alias;
  device.fingerprint = manual.fingerprint;
  device.version = "2.1";
  return device;
}

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

void sleep_for_interval(std::chrono::milliseconds duration) {
#if LOCALSEND_PLATFORM_PSV
  ::usleep(static_cast<useconds_t>(duration.count() * 1000));
#else
  std::this_thread::sleep_for(duration);
#endif
}

} // namespace

AppService::AppService(AppConfig config, AppServiceOptions options)
    : config_(std::move(config)), options_(std::move(options)), self_(make_self_info()) {
  load_configured_manual_devices();
}

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
  status.last_send_error = last_send_error();
  status.send_status_message = send_status_message();
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

bool AppService::update_config(AppConfig config) {
  if (server_running() || discovery_running() || send_running()) {
    return false;
  }

  config_ = std::move(config);
  self_ = make_self_info();
  devices_.clear();
  load_configured_manual_devices();
  return true;
}

bool AppService::save_config() const {
  return save_config_as(config_.config_path);
}

bool AppService::save_config_as(const std::filesystem::path& path) const {
  try {
    localsend::save_config(config_, path);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool AppService::start_server() {
  if (server_) {
    return true;
  }

  set_last_server_error("");
  self_ = make_self_info();
  try {
    if (options_.enable_tls) {
#if LOCALSEND_HAS_MBEDTLS
      const TlsIdentity identity = load_or_create_tls_identity(config_.certificate_path, config_.private_key_path);
      self_.protocol = ProtocolType::Https;
      self_.fingerprint = identity.fingerprint;
      server_ = std::make_unique<LocalSendServer>(self_,
                                                  config_.inbox_path,
                                                  TlsCredentials{identity.certificate_pem, identity.private_key_pem},
                                                  &transfers_);
#else
      set_last_server_error("TLS support is not compiled in");
      return false;
#endif
    } else {
      self_.protocol = ProtocolType::Http;
      self_.fingerprint.clear();
      server_ = std::make_unique<LocalSendServer>(self_, config_.inbox_path, &transfers_);
    }
  } catch (const std::exception& e) {
    set_last_server_error(e.what());
    server_.reset();
    return false;
  }

  if (!server_->start(config_.port)) {
    set_last_server_error("listen failed on port " + std::to_string(config_.port));
    server_.reset();
    return false;
  }
  self_.port = server_->port();
  server_->set_register_callback([this](Device device) {
    if (!is_self_device(device)) {
      devices_.upsert_discovered(std::move(device));
    }
  });
  return true;
}

std::string AppService::last_server_error() const {
  std::lock_guard<std::mutex> lock(server_status_mutex_);
  return last_server_error_;
}

void AppService::stop_server() {
  if (!server_) {
    return;
  }
  server_->stop();
  server_.reset();
}

void AppService::poll_server_once() {
  if (server_) {
    server_->poll_once();
  }
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
  devices_.mark_stale_offline(kDiscoveryStaleAge);
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
#if LOCALSEND_PLATFORM_PSV
  discovery_interval_ = interval;
  discovery_scan_timeout_ = scan_timeout;
  if (::pthread_create(&discovery_thread_, nullptr, &AppService::discovery_thread_entry, this) != 0) {
    discovery_running_ = false;
    return false;
  }
  discovery_thread_started_ = true;
#else
  discovery_thread_ = std::thread(&AppService::discovery_loop, this, interval, scan_timeout);
#endif
  return true;
}

void AppService::stop_discovery() {
  if (!discovery_running_) {
    return;
  }
  discovery_running_ = false;
#if LOCALSEND_PLATFORM_PSV
  if (discovery_thread_started_) {
    ::pthread_join(discovery_thread_, nullptr);
    discovery_thread_started_ = false;
  }
#else
  if (discovery_thread_.joinable()) {
    discovery_thread_.join();
  }
#endif
}

std::string AppService::add_manual_device(std::string ip,
                                          int port,
                                          bool https,
                                          std::string alias,
                                          std::string fingerprint) {
  AppConfig::ManualDevice manual;
  manual.ip = ip;
  manual.port = port;
  manual.https = https;
  manual.alias = alias;
  manual.fingerprint = fingerprint;

  auto existing = std::find_if(config_.manual_devices.begin(),
                               config_.manual_devices.end(),
                               [&manual](const AppConfig::ManualDevice& device) {
                                 return same_manual_device(device, manual);
                               });
  std::string previous_key;
  if (existing != config_.manual_devices.end()) {
    previous_key = device_key(device_from_manual(*existing));
    *existing = manual;
  } else {
    config_.manual_devices.push_back(manual);
  }

  Device device = device_from_manual(manual);
  const std::string key = device_key(device);
  if (!previous_key.empty() && previous_key != key) {
    devices_.remove(previous_key);
  }
  return devices_.upsert_manual(std::move(device));
}

bool AppService::remove_manual_device(const std::string& device_key) {
  const std::optional<DeviceEntry> entry = devices_.get(device_key);
  if (!entry || entry->source != DeviceSource::Manual) {
    return false;
  }

  const AppConfig::ManualDevice manual{
      entry->device.ip,
      entry->device.port,
      entry->device.https,
      entry->device.alias,
      entry->device.fingerprint,
  };
  const auto old_size = config_.manual_devices.size();
  config_.manual_devices.erase(std::remove_if(config_.manual_devices.begin(),
                                              config_.manual_devices.end(),
                                              [&manual](const AppConfig::ManualDevice& device) {
                                                return same_manual_device(device, manual);
                                              }),
                               config_.manual_devices.end());
  if (config_.manual_devices.size() == old_size) {
    return false;
  }

  devices_.remove(device_key);
  return true;
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
    set_last_send_error("send already running");
    return false;
  }

#if LOCALSEND_PLATFORM_PSV
  if (send_thread_started_) {
    ::pthread_join(send_thread_, nullptr);
    send_thread_started_ = false;
  }
#else
  if (send_thread_.joinable()) {
    send_thread_.join();
  }
#endif

  const std::optional<DeviceEntry> entry = devices_.get(device_key);
  if (!entry) {
    set_last_send_error("selected peer disappeared");
    return false;
  }
  if (file_paths.empty()) {
    set_last_send_error("no files selected");
    return false;
  }

  send_control_.cancel_requested = false;
  send_control_.status_callback = [this](const std::string& message) {
    set_send_status_message(message);
  };
  set_last_send_error("");
  set_send_status_message("Starting send");
  send_running_ = true;
#if LOCALSEND_PLATFORM_PSV
  send_device_ = entry->device;
  send_file_paths_ = std::move(file_paths);
  if (::pthread_create(&send_thread_, nullptr, &AppService::send_thread_entry, this) != 0) {
    send_running_ = false;
    send_file_paths_.clear();
    set_last_send_error("failed to create send thread");
    return false;
  }
  send_thread_started_ = true;
#else
  send_thread_ = std::thread(&AppService::send_worker, this, entry->device, std::move(file_paths));
#endif
  return true;
}

bool AppService::cancel_current_send() {
  if (!send_running_) {
    set_last_send_error("no active send");
    return false;
  }
  send_control_.cancel_requested = true;
  set_last_send_error("send cancellation requested");
  return true;
}

std::string AppService::last_send_error() const {
  std::lock_guard<std::mutex> lock(send_status_mutex_);
  return last_send_error_;
}

std::string AppService::send_status_message() const {
  std::lock_guard<std::mutex> lock(send_status_mutex_);
  return send_status_message_;
}

void AppService::set_last_send_error(std::string error) {
  std::lock_guard<std::mutex> lock(send_status_mutex_);
  last_send_error_ = std::move(error);
}

void AppService::set_send_status_message(std::string message) {
  std::lock_guard<std::mutex> lock(send_status_mutex_);
  send_status_message_ = std::move(message);
}

void AppService::set_last_server_error(std::string error) {
  std::lock_guard<std::mutex> lock(server_status_mutex_);
  last_server_error_ = std::move(error);
}

void AppService::wait_for_send_idle() {
#if LOCALSEND_PLATFORM_PSV
  if (send_thread_started_) {
    ::pthread_join(send_thread_, nullptr);
    send_thread_started_ = false;
  }
#else
  if (send_thread_.joinable()) {
    send_thread_.join();
  }
#endif
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

void AppService::load_configured_manual_devices() {
  for (const auto& manual : config_.manual_devices) {
    devices_.upsert_manual(device_from_manual(manual));
  }
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
  const std::chrono::milliseconds burst_delays[] = {
      std::chrono::milliseconds(100),
      std::chrono::milliseconds(500),
      std::chrono::milliseconds(2000),
  };

  while (discovery_running_) {
    for (const auto delay : burst_delays) {
      sleep_for_interval(delay);
      if (!discovery_running_) {
        return;
      }
      announce_once();
    }
    refresh_discovery(bounded_timeout);

    const auto sleep_step = std::chrono::milliseconds(50);
    auto slept = std::chrono::milliseconds(0);
    while (discovery_running_ && slept < bounded_interval) {
      const auto remaining = bounded_interval - slept;
      const auto current_sleep = std::min(sleep_step, remaining);
      sleep_for_interval(current_sleep);
      slept += current_sleep;
    }
  }
}

void AppService::send_worker(Device device, std::vector<std::filesystem::path> file_paths) {
  std::optional<TlsIdentity> identity;
  std::optional<TlsCredentials> client_credentials;
  if (device.https) {
    try {
      set_send_status_message("Loading HTTPS identity");
      identity = load_or_create_tls_identity(config_.certificate_path, config_.private_key_path);
      client_credentials = TlsCredentials{identity->certificate_pem, identity->private_key_pem};
    } catch (const std::exception& e) {
      set_last_send_error(std::string("failed to load HTTPS identity: ") + e.what());
      set_send_status_message("");
      send_running_ = false;
      return;
    }
  }
  const SendFilesResult result = send_files_http_detailed(device,
                                                          file_paths,
                                                          self_,
                                                          &transfers_,
                                                          &send_control_,
                                                          client_credentials ? &*client_credentials : nullptr);
  set_last_send_error(result.ok ? "" : result.error);
  set_send_status_message(result.ok ? "Send complete" : "");
  send_running_ = false;
}

#if LOCALSEND_PLATFORM_PSV
void* AppService::discovery_thread_entry(void* arg) {
  auto* service = static_cast<AppService*>(arg);
  service->discovery_loop(service->discovery_interval_, service->discovery_scan_timeout_);
  return nullptr;
}

void* AppService::send_thread_entry(void* arg) {
  auto* service = static_cast<AppService*>(arg);
  service->send_worker(service->send_device_, std::move(service->send_file_paths_));
  service->send_file_paths_.clear();
  return nullptr;
}
#endif

} // namespace localsend
