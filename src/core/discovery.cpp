#include "localsend/discovery.hpp"

#include "localsend/constants.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace localsend {
namespace {

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

Device to_device(const MulticastDto& dto, const std::string& ip, int fallback_port) {
  Device device;
  device.ip = ip;
  device.version = dto.version;
  device.port = dto.port > 0 ? dto.port : fallback_port;
  device.https = dto.protocol == ProtocolType::Https;
  device.fingerprint = dto.fingerprint;
  device.alias = dto.alias;
  device.device_model = dto.device_model;
  device.device_type = dto.device_type;
  device.download = dto.download;
  return device;
}

int create_udp_socket() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }

  int enabled = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  return fd;
}

} // namespace

std::string make_multicast_announcement(const MulticastDto& self) {
  return to_json(self).dump();
}

Device device_from_multicast(const std::string& payload, const std::string& ip, int fallback_port) {
  return to_device(multicast_from_json(Json::parse(payload)), ip, fallback_port);
}

bool announce_multicast(const MulticastDto& self, const std::string& group, int port) {
  const int fd = create_udp_socket();
  if (fd < 0) {
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, group.c_str(), &addr.sin_addr) != 1) {
    close_fd(fd);
    return false;
  }

  const std::string payload = make_multicast_announcement(self);
  const ssize_t sent = ::sendto(fd,
                                payload.data(),
                                payload.size(),
                                0,
                                reinterpret_cast<sockaddr*>(&addr),
                                sizeof(addr));
  close_fd(fd);
  return sent == static_cast<ssize_t>(payload.size());
}

std::vector<Device> discover_peers(std::chrono::milliseconds timeout, const std::string& group, int port) {
  std::vector<Device> devices;
  const int fd = create_udp_socket();
  if (fd < 0) {
    return devices;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(fd);
    return devices;
  }

  ip_mreq mreq{};
  if (::inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
    close_fd(fd);
    return devices;
  }
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
    close_fd(fd);
    return devices;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::map<std::string, Device> by_endpoint;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    timeval tv{};
    tv.tv_sec = static_cast<long>(remaining.count() / 1000);
    tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    const int ready = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) {
      break;
    }

    char buffer[8192];
    sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);
    const ssize_t got = ::recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&sender), &sender_len);
    if (got <= 0) {
      continue;
    }

    char ip[INET_ADDRSTRLEN] = {};
    if (!::inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip))) {
      continue;
    }

    try {
      Device device = device_from_multicast(std::string(buffer, static_cast<std::size_t>(got)), ip, port);
      by_endpoint[device.ip + ":" + std::to_string(device.port)] = std::move(device);
    } catch (const std::exception&) {
      continue;
    }
  }

  close_fd(fd);
  for (auto& entry : by_endpoint) {
    devices.push_back(std::move(entry.second));
  }
  return devices;
}

} // namespace localsend

