#include "localsend/constants.hpp"
#include "localsend/discovery.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* argv0) {
  std::cout << "Usage:\n"
            << "  " << argv0 << " info [alias]\n"
            << "  " << argv0 << " serve [inbox] [port] [alias]\n"
            << "  " << argv0 << " send <ip> <port> <file> [alias]\n"
            << "  " << argv0 << " discover [milliseconds]\n";
}

localsend::InfoRegisterDto make_self(const std::string& alias, int port = localsend::kDefaultPort) {
  localsend::InfoRegisterDto self;
  self.alias = alias;
  self.port = port;
  self.protocol = localsend::ProtocolType::Http;
  self.fingerprint = "";
  self.device_model = "Desktop prototype";
  self.device_type = localsend::DeviceType::Desktop;
  self.download = false;
  return self;
}

int command_info(int argc, char** argv) {
  const std::string alias = argc >= 3 ? argv[2] : "LocalSend Handheld Desktop";
  localsend::MulticastDto self;
  static_cast<localsend::InfoRegisterDto&>(self) = make_self(alias);
  self.announce = true;
  std::cout << localsend::to_json(self).dump() << '\n';
  return 0;
}

int command_serve(int argc, char** argv) {
  const std::filesystem::path inbox = argc >= 3 ? argv[2] : "inbox";
  const int port = argc >= 4 ? std::stoi(argv[3]) : localsend::kDefaultPort;
  const std::string alias = argc >= 5 ? argv[4] : "LocalSend Handheld Desktop";

  localsend::LocalSendServer server(make_self(alias, port), inbox);
  if (!server.start(port)) {
    std::cerr << "failed to start server on port " << port << '\n';
    return 1;
  }

  localsend::MulticastDto announce;
  static_cast<localsend::InfoRegisterDto&>(announce) = make_self(alias, server.port());
  announce.announce = true;
  localsend::announce_multicast(announce);

  std::cout << "serving HTTP LocalSend v2.1 on port " << server.port() << '\n'
            << "inbox: " << std::filesystem::absolute(inbox) << '\n'
            << "disable Encryption on official LocalSend peers\n"
            << "press Enter to stop\n";
  std::string line;
  std::getline(std::cin, line);
  server.stop();
  return 0;
}

int command_send(int argc, char** argv) {
  if (argc < 5) {
    print_usage(argv[0]);
    return 1;
  }

  localsend::Device target;
  target.ip = argv[2];
  target.port = std::stoi(argv[3]);
  target.https = false;

  const std::filesystem::path file = argv[4];
  const std::string alias = argc >= 6 ? argv[5] : "LocalSend Handheld Desktop";
  if (!localsend::send_single_file_http(target, file, make_self(alias))) {
    std::cerr << "send failed\n";
    return 1;
  }
  std::cout << "sent " << file << " to " << target.ip << ':' << target.port << '\n';
  return 0;
}

int command_discover(int argc, char** argv) {
  const auto timeout = std::chrono::milliseconds(argc >= 3 ? std::stoi(argv[2]) : 500);
  const auto peers = localsend::discover_peers(timeout);
  for (const auto& peer : peers) {
    std::cout << peer.alias << " http://" << peer.ip << ':' << peer.port << " version=" << peer.version << '\n';
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return command_info(argc, argv);
  }

  const std::string command = argv[1];
  if (command == "info") {
    return command_info(argc, argv);
  }
  if (command == "serve") {
    return command_serve(argc, argv);
  }
  if (command == "send") {
    return command_send(argc, argv);
  }
  if (command == "discover") {
    return command_discover(argc, argv);
  }

  print_usage(argv[0]);
  return 1;
}
