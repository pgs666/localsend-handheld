#pragma once

#include "localsend/device_store.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace localsend {

std::optional<std::size_t> first_online_device_index(const std::vector<DeviceEntry>& devices);
std::optional<std::size_t> next_online_device_index(const std::vector<DeviceEntry>& devices,
                                                    std::optional<std::size_t> current_index);
std::optional<DeviceEntry> selected_online_device(const std::vector<DeviceEntry>& devices,
                                                  std::optional<std::size_t> current_index);
std::string format_selected_device(const std::vector<DeviceEntry>& devices,
                                   std::optional<std::size_t> current_index);

} // namespace localsend
