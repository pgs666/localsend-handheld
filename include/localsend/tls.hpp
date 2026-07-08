#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace localsend {

struct TlsCredentials {
  std::string certificate_pem;
  std::string private_key_pem;
};

class TlsConnection {
public:
  ~TlsConnection();

  TlsConnection(TlsConnection&&) noexcept;
  TlsConnection& operator=(TlsConnection&&) noexcept;

  TlsConnection(const TlsConnection&) = delete;
  TlsConnection& operator=(const TlsConnection&) = delete;

  static TlsConnection client(int fd, const TlsCredentials* credentials = nullptr);
  static TlsConnection server(int fd, const TlsCredentials& credentials);

  bool handshake();
  int read(std::uint8_t* buffer, std::size_t size);
  bool write_all(const std::uint8_t* data, std::size_t size);
  void close_notify();
  std::string peer_fingerprint() const;

private:
  struct Impl;
  explicit TlsConnection(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace localsend
