#include "localsend/device_selection.hpp"

namespace localsend {

std::optional<std::size_t> first_online_device_index(const std::vector<DeviceEntry>& devices) {
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (devices[i].online) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> next_online_device_index(const std::vector<DeviceEntry>& devices,
                                                    std::optional<std::size_t> current_index) {
  if (devices.empty()) {
    return std::nullopt;
  }

  const std::size_t start = current_index && *current_index < devices.size() ? (*current_index + 1) : 0;
  for (std::size_t offset = 0; offset < devices.size(); ++offset) {
    const std::size_t index = (start + offset) % devices.size();
    if (devices[index].online) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<DeviceEntry> selected_online_device(const std::vector<DeviceEntry>& devices,
                                                  std::optional<std::size_t> current_index) {
  if (current_index && *current_index < devices.size() && devices[*current_index].online) {
    return devices[*current_index];
  }

  const auto fallback = first_online_device_index(devices);
  if (!fallback) {
    return std::nullopt;
  }
  return devices[*fallback];
}

std::string format_selected_device(const std::vector<DeviceEntry>& devices,
                                   std::optional<std::size_t> current_index) {
  const auto selected = selected_online_device(devices, current_index);
  if (!selected) {
    return "No online peer selected";
  }

  const Device& device = selected->device;
  std::string text = device.alias.empty() ? "Unnamed peer" : device.alias;
  text += " ";
  text += device.https ? "[https] " : "[http] ";
  text += device.ip + ":" + std::to_string(device.port);
  text += " (";
  text += to_string(selected->source);
  text += ")";
  return text;
}

} // namespace localsend
