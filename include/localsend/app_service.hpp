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
#include <optional>
#include <string>
#include <thread>
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
  bool https = false;
  std::string alias;
  std::string fingerprint;
  int port = 0;
  std::size_t device_count = 0;
  std::size_t transfer_count = 0;
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

  DeviceStore& devices() { return devices_; }
  const DeviceStore& devices() const { return devices_; }
  TransferStore& transfers() { return transfers_; }
  const TransferStore& transfers() const { return transfers_; }

  bool start_server();
  void stop_server();
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

  bool send_files_to_device(const std::string& device_key, const std::vector<std::filesystem::path>& file_paths);

private:
  InfoRegisterDto make_self_info() const;
  bool is_self_device(const Device& device) const;
  void discovery_loop(std::chrono::milliseconds interval, std::chrono::milliseconds scan_timeout);

  AppConfig config_;
  AppServiceOptions options_;
  InfoRegisterDto self_;
  DeviceStore devices_;
  TransferStore transfers_;
  std::unique_ptr<LocalSendServer> server_;
  std::atomic<bool> discovery_running_{false};
  std::thread discovery_thread_;
};

} // namespace localsend
