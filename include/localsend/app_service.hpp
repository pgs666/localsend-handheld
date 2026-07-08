#pragma once

#include "localsend/config.hpp"
#include "localsend/device_store.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"
#include "localsend/transfer.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#if LOCALSEND_PLATFORM_PSV
#include <pthread.h>
#else
#include <thread>
#endif
#include <vector>

namespace localsend {

struct AppServiceOptions {
  PlatformKind platform = PlatformKind::Desktop;
  bool enable_tls = true;
  std::string device_model;
  DeviceType device_type = DeviceType::Desktop;
};

struct AppServiceStatus {
  bool server_running = false;
  bool discovery_running = false;
  bool send_running = false;
  bool https = false;
  std::string alias;
  std::string fingerprint;
  std::string last_send_error;
  std::string send_status_message;
  int port = 0;
  std::size_t device_count = 0;
  std::size_t transfer_count = 0;
};

struct AppSnapshot {
  AppServiceStatus status;
  InfoRegisterDto self;
  std::vector<DeviceEntry> devices;
  std::vector<TransferItem> transfers;
};

class AppService {
public:
  explicit AppService(AppConfig config, AppServiceOptions options = {});
  ~AppService();

  AppService(const AppService&) = delete;
  AppService& operator=(const AppService&) = delete;

  const AppConfig& config() const { return config_; }
  const InfoRegisterDto& self_info() const { return self_; }
  AppServiceStatus status() const;
  AppSnapshot snapshot() const;
  bool update_config(AppConfig config);
  bool save_config() const;
  bool save_config_as(const std::filesystem::path& path) const;

  DeviceStore& devices() { return devices_; }
  const DeviceStore& devices() const { return devices_; }
  TransferStore& transfers() { return transfers_; }
  const TransferStore& transfers() const { return transfers_; }

  bool start_server();
  void stop_server();
  void poll_server_once();
  bool server_running() const { return server_ != nullptr; }

  bool announce_once() const;
  int refresh_discovery(std::chrono::milliseconds timeout);
  bool start_discovery(std::chrono::milliseconds interval = std::chrono::seconds(5),
                       std::chrono::milliseconds scan_timeout = std::chrono::milliseconds(500));
  void stop_discovery();
  bool discovery_running() const { return discovery_running_; }
  std::string add_manual_device(std::string ip,
                                int port,
                                bool https,
                                std::string alias = "",
                                std::string fingerprint = "");
  bool remove_manual_device(const std::string& device_key);

  bool send_files_to_device(const std::string& device_key, const std::vector<std::filesystem::path>& file_paths);
  bool start_send_to_device(const std::string& device_key, std::vector<std::filesystem::path> file_paths);
  bool cancel_current_send();
  void wait_for_send_idle();
  bool send_running() const { return send_running_; }
  std::string last_send_error() const;
  std::string send_status_message() const;

private:
  InfoRegisterDto make_self_info() const;
  void load_configured_manual_devices();
  bool is_self_device(const Device& device) const;
  void set_last_send_error(std::string error);
  void set_send_status_message(std::string message);
  void discovery_loop(std::chrono::milliseconds interval, std::chrono::milliseconds scan_timeout);
  void send_worker(Device device, std::vector<std::filesystem::path> file_paths);
#if LOCALSEND_PLATFORM_PSV
  static void* discovery_thread_entry(void* arg);
  static void* send_thread_entry(void* arg);
#endif

  AppConfig config_;
  AppServiceOptions options_;
  InfoRegisterDto self_;
  DeviceStore devices_;
  TransferStore transfers_;
  std::unique_ptr<LocalSendServer> server_;
  std::atomic<bool> discovery_running_{false};
  std::atomic<bool> send_running_{false};
  SendFilesControl send_control_;
  mutable std::mutex send_status_mutex_;
  std::string last_send_error_;
  std::string send_status_message_;
#if LOCALSEND_PLATFORM_PSV
  pthread_t discovery_thread_{};
  pthread_t send_thread_{};
  bool discovery_thread_started_ = false;
  bool send_thread_started_ = false;
  std::chrono::milliseconds discovery_interval_{std::chrono::seconds(5)};
  std::chrono::milliseconds discovery_scan_timeout_{std::chrono::milliseconds(500)};
  Device send_device_;
  std::vector<std::filesystem::path> send_file_paths_;
#else
  std::thread discovery_thread_;
  std::thread send_thread_;
#endif
};

} // namespace localsend
