#pragma once

#include "localsend/device_store.hpp"
#include "localsend/transfer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace localsend {

std::string format_device_summary(const std::vector<DeviceEntry>& devices, std::size_t max_items = 3);
std::string format_transfer_summary(const std::vector<TransferItem>& transfers, std::size_t max_items = 3);

} // namespace localsend
