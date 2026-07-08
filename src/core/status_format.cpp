#include "localsend/status_format.hpp"

#include <algorithm>
#include <sstream>

namespace localsend {
namespace {

std::string protocol_label(const Device& device) {
  return device.https ? "https" : "http";
}

int progress_percent(const TransferItem& item) {
  if (item.size == 0) {
    return item.status == TransferStatus::Completed ? 100 : 0;
  }
  const auto clamped = std::min(item.bytes_transferred, item.size);
  return static_cast<int>((clamped * 100) / item.size);
}

} // namespace

std::string format_device_summary(const std::vector<DeviceEntry>& devices, std::size_t max_items) {
  if (devices.empty() || max_items == 0) {
    return "No peers yet";
  }

  std::ostringstream out;
  const std::size_t count = std::min(max_items, devices.size());
  for (std::size_t i = 0; i < count; ++i) {
    const DeviceEntry& entry = devices[i];
    const Device& device = entry.device;
    if (i > 0) {
      out << '\n';
    }
    out << (entry.online ? "online " : "offline ");
    out << (device.alias.empty() ? "Unnamed peer" : device.alias);
    out << " [" << protocol_label(device) << "] ";
    out << device.ip << ':' << device.port;
    out << " (" << to_string(entry.source) << ')';
  }
  if (devices.size() > count) {
    out << "\n+" << (devices.size() - count) << " more";
  }
  return out.str();
}

std::string format_transfer_summary(const std::vector<TransferItem>& transfers, std::size_t max_items) {
  if (transfers.empty() || max_items == 0) {
    return "No transfers yet";
  }

  std::ostringstream out;
  const std::size_t count = std::min(max_items, transfers.size());
  for (std::size_t i = 0; i < count; ++i) {
    const TransferItem& item = transfers[transfers.size() - 1 - i];
    if (i > 0) {
      out << '\n';
    }
    out << to_string(item.direction) << ' ';
    out << item.file_name << " -> ";
    out << (item.peer_alias.empty() ? item.peer_ip : item.peer_alias);
    out << " [" << to_string(item.status) << ", " << progress_percent(item) << "%]";
    if (!item.error.empty()) {
      out << ' ' << item.error;
    }
  }
  if (transfers.size() > count) {
    out << "\n+" << (transfers.size() - count) << " older";
  }
  return out.str();
}

} // namespace localsend
