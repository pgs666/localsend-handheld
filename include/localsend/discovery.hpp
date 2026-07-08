#pragma once

#include "localsend/protocol.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace localsend {

std::string make_multicast_announcement(const MulticastDto& self);
Device device_from_multicast(const std::string& payload, const std::string& ip, int fallback_port);

bool announce_multicast(const MulticastDto& self,
                        const std::string& group = "224.0.0.167",
                        int port = 53317);

std::vector<Device> discover_peers(std::chrono::milliseconds timeout,
                                   const std::string& group = "224.0.0.167",
                                   int port = 53317);

} // namespace localsend

