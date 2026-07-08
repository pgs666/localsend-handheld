#include "localsend/device_store.hpp"

#include <algorithm>
#include <utility>

namespace localsend {

namespace {

std::string endpoint_key(const Device& device) {
  return device.ip + ":" + std::to_string(device.port);
}

} // namespace

std::string DeviceStore::upsert_discovered(Device device) {
  return upsert(std::move(device), DeviceSource::Discovered);
}

std::string DeviceStore::upsert_manual(Device device) {
  return upsert(std::move(device), DeviceSource::Manual);
}

bool DeviceStore::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto old_size = devices_.size();
  devices_.erase(std::remove_if(devices_.begin(),
                                devices_.end(),
                                [&key](const DeviceEntry& entry) {
                                  return entry.key == key;
                                }),
                 devices_.end());
  return devices_.size() != old_size;
}

bool DeviceStore::mark_offline(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  DeviceEntry* entry = find_locked(key);
  if (!entry) {
    return false;
  }
  entry->online = false;
  return true;
}

int DeviceStore::mark_stale_offline(std::chrono::seconds max_age) {
  const auto now = std::chrono::system_clock::now();
  int changed = 0;

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : devices_) {
    if (entry.online && now - entry.last_seen > max_age) {
      entry.online = false;
      ++changed;
    }
  }
  return changed;
}

std::optional<DeviceEntry> DeviceStore::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const DeviceEntry* entry = find_locked(key);
  if (!entry) {
    return std::nullopt;
  }
  return *entry;
}

std::vector<DeviceEntry> DeviceStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_;
}

void DeviceStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  devices_.clear();
}

std::string DeviceStore::upsert(Device device, DeviceSource source) {
  const auto now = std::chrono::system_clock::now();
  const std::string key = device_key(device);

  std::lock_guard<std::mutex> lock(mutex_);
  DeviceEntry* existing = find_locked(key);
  if (existing) {
    existing->device = std::move(device);
    if (source == DeviceSource::Manual) {
      existing->source = DeviceSource::Manual;
    }
    existing->online = true;
    existing->last_seen = now;
    return key;
  }

  DeviceEntry entry;
  entry.key = key;
  entry.device = std::move(device);
  entry.source = source;
  entry.online = true;
  entry.first_seen = now;
  entry.last_seen = now;
  devices_.push_back(std::move(entry));
  return key;
}

DeviceEntry* DeviceStore::find_locked(const std::string& key) {
  const auto it = std::find_if(devices_.begin(), devices_.end(), [&key](const DeviceEntry& entry) {
    return entry.key == key;
  });
  return it == devices_.end() ? nullptr : &*it;
}

const DeviceEntry* DeviceStore::find_locked(const std::string& key) const {
  const auto it = std::find_if(devices_.begin(), devices_.end(), [&key](const DeviceEntry& entry) {
    return entry.key == key;
  });
  return it == devices_.end() ? nullptr : &*it;
}

std::string device_key(const Device& device) {
  if (!device.fingerprint.empty()) {
    return "fingerprint:" + device.fingerprint;
  }
  return "endpoint:" + endpoint_key(device);
}

const char* to_string(DeviceSource source) {
  switch (source) {
  case DeviceSource::Discovered:
    return "discovered";
  case DeviceSource::Manual:
    return "manual";
  }
  return "discovered";
}

} // namespace localsend
