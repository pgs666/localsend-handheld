#include "localsend/constants.hpp"
#include "localsend/protocol.hpp"

#include <iostream>

int main() {
  localsend::MulticastDto self;
  self.alias = "LocalSend Handheld Desktop";
  self.port = localsend::kDefaultPort;
  self.protocol = localsend::ProtocolType::Http;

  std::cout << localsend::to_json(self).dump() << '\n';
  std::cout << "HTTP mode only: disable Encryption in official LocalSend peers.\n";
  return 0;
}

