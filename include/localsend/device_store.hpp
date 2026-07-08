#pragma once

#include "localsend/protocol.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace localsend {

enum class DeviceSource {
  Discovered,
  Manual,
};

struct DeviceEntry {
  std::string key;
  Device device;
  DeviceSource source = DeviceSource::Discovered;
  bool online = true;
  std::chrono::system_clock::time_point first_seen{};
  std::chrono::system_clock::time_point last_seen{};
};

class DeviceStore {
public:
  std::string upsert_discovered(Device device);
  std::string upsert_manual(Device device);

  bool remove(const std::string& key);
  bool mark_offline(const std::string& key);
  int mark_stale_offline(std::chrono::seconds max_age);

  std::optional<DeviceEntry> get(const std::string& key) const;
  std::vector<DeviceEntry> snapshot() const;
  void clear();

private:
  std::string upsert(Device device, DeviceSource source);
  DeviceEntry* find_locked(const std::string& key);
  const DeviceEntry* find_locked(const std::string& key) const;

  mutable std::mutex mutex_;
  std::vector<DeviceEntry> devices_;
};

std::string device_key(const Device& device);
const char* to_string(DeviceSource source);

} // namespace localsend
