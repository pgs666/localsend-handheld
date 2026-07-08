#include "localsend/tls.hpp"

#include "localsend/security.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace localsend {
namespace {

int tls_send(void* ctx, const unsigned char* buffer, std::size_t size) {
  const int fd = *static_cast<int*>(ctx);
#ifdef MSG_NOSIGNAL
  const ssize_t sent = ::send(fd, buffer, size, MSG_NOSIGNAL);
#else
  const ssize_t sent = ::send(fd, buffer, size, 0);
#endif
  if (sent < 0) {
    if (errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return static_cast<int>(sent);
}

int tls_recv(void* ctx, unsigned char* buffer, std::size_t size) {
  const int fd = *static_cast<int*>(ctx);
  const ssize_t got = ::recv(fd, buffer, size, 0);
  if (got < 0) {
    if (errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  if (got == 0) {
    return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
  }
  return static_cast<int>(got);
}

bool retryable_tls_result(int result) {
  return result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE;
}

std::string mbedtls_error_message(const char* operation, int result) {
  char buffer[160] = {};
  mbedtls_strerror(result, buffer, sizeof(buffer));
  std::string text(operation);
  text += " failed: ";
  text += std::to_string(result);
  if (buffer[0] != '\0') {
    text += " (";
    text += buffer;
    text += ")";
  }
  return text;
}

} // namespace

struct TlsConnection::Impl {
  int fd = -1;
  bool owns_fd = true;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509_crt certificate;
  mbedtls_pk_context private_key;
  std::string last_error;

  Impl() {
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);
  }

  ~Impl() {
    if (owns_fd && fd >= 0) {
      ::close(fd);
    }
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
};

TlsConnection::TlsConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TlsConnection::~TlsConnection() = default;

TlsConnection::TlsConnection(TlsConnection&&) noexcept = default;
TlsConnection& TlsConnection::operator=(TlsConnection&&) noexcept = default;

TlsConnection TlsConnection::client(int fd, const TlsCredentials* credentials) {
  auto impl = std::make_unique<Impl>();
  impl->fd = fd;

  const char* personalization = "localsend-handheld-client";
  if (mbedtls_ctr_drbg_seed(&impl->ctr_drbg,
                            mbedtls_entropy_func,
                            &impl->entropy,
                            reinterpret_cast<const unsigned char*>(personalization),
                            std::strlen(personalization)) != 0) {
    throw std::runtime_error("mbedTLS client RNG seed failed");
  }

  if (mbedtls_ssl_config_defaults(&impl->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    throw std::runtime_error("mbedTLS client config failed");
  }
  mbedtls_ssl_conf_authmode(&impl->config, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&impl->config, mbedtls_ctr_drbg_random, &impl->ctr_drbg);

  if (credentials) {
    if (mbedtls_x509_crt_parse(&impl->certificate,
                               reinterpret_cast<const unsigned char*>(credentials->certificate_pem.c_str()),
                               credentials->certificate_pem.size() + 1) != 0) {
      throw std::runtime_error("mbedTLS client certificate parse failed");
    }
    const unsigned char* key_data = reinterpret_cast<const unsigned char*>(credentials->private_key_pem.c_str());
    const std::size_t key_size = credentials->private_key_pem.size() + 1;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    if (mbedtls_pk_parse_key(&impl->private_key,
                             key_data,
                             key_size,
                             nullptr,
                             0,
                             mbedtls_ctr_drbg_random,
                             &impl->ctr_drbg) != 0) {
#else
    if (mbedtls_pk_parse_key(&impl->private_key,
                             key_data,
                             key_size,
                             nullptr,
                             0) != 0) {
#endif
      throw std::runtime_error("mbedTLS client private key parse failed");
    }
    if (mbedtls_ssl_conf_own_cert(&impl->config, &impl->certificate, &impl->private_key) != 0) {
      throw std::runtime_error("mbedTLS client certificate config failed");
    }
  }

  if (mbedtls_ssl_setup(&impl->ssl, &impl->config) != 0) {
    throw std::runtime_error("mbedTLS client setup failed");
  }
  mbedtls_ssl_set_bio(&impl->ssl, &impl->fd, tls_send, tls_recv, nullptr);
  return TlsConnection(std::move(impl));
}

TlsConnection TlsConnection::server(int fd, const TlsCredentials& credentials) {
  auto impl = std::make_unique<Impl>();
  impl->fd = fd;

  const char* personalization = "localsend-handheld-server";
  if (mbedtls_ctr_drbg_seed(&impl->ctr_drbg,
                            mbedtls_entropy_func,
                            &impl->entropy,
                            reinterpret_cast<const unsigned char*>(personalization),
                            std::strlen(personalization)) != 0) {
    throw std::runtime_error("mbedTLS server RNG seed failed");
  }

  if (mbedtls_x509_crt_parse(&impl->certificate,
                             reinterpret_cast<const unsigned char*>(credentials.certificate_pem.c_str()),
                             credentials.certificate_pem.size() + 1) != 0) {
    throw std::runtime_error("mbedTLS certificate parse failed");
  }
  const unsigned char* key_data = reinterpret_cast<const unsigned char*>(credentials.private_key_pem.c_str());
  const std::size_t key_size = credentials.private_key_pem.size() + 1;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  if (mbedtls_pk_parse_key(&impl->private_key,
                           key_data,
                           key_size,
                           nullptr,
                           0,
                           mbedtls_ctr_drbg_random,
                           &impl->ctr_drbg) != 0) {
#else
  if (mbedtls_pk_parse_key(&impl->private_key,
                           key_data,
                           key_size,
                           nullptr,
                           0) != 0) {
#endif
    throw std::runtime_error("mbedTLS private key parse failed");
  }

  if (mbedtls_ssl_config_defaults(&impl->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    throw std::runtime_error("mbedTLS server config failed");
  }
  mbedtls_ssl_conf_rng(&impl->config, mbedtls_ctr_drbg_random, &impl->ctr_drbg);
  if (mbedtls_ssl_conf_own_cert(&impl->config, &impl->certificate, &impl->private_key) != 0) {
    throw std::runtime_error("mbedTLS own certificate config failed");
  }

  if (mbedtls_ssl_setup(&impl->ssl, &impl->config) != 0) {
    throw std::runtime_error("mbedTLS server setup failed");
  }
  mbedtls_ssl_set_bio(&impl->ssl, &impl->fd, tls_send, tls_recv, nullptr);
  return TlsConnection(std::move(impl));
}

bool TlsConnection::handshake() {
  int result = 0;
  while ((result = mbedtls_ssl_handshake(&impl_->ssl)) != 0) {
    if (!retryable_tls_result(result)) {
      impl_->last_error = mbedtls_error_message("TLS handshake", result);
      return false;
    }
  }
  impl_->last_error.clear();
  return true;
}

int TlsConnection::read(std::uint8_t* buffer, std::size_t size) {
  while (true) {
    const int result = mbedtls_ssl_read(&impl_->ssl, buffer, size);
    if (retryable_tls_result(result)) {
      continue;
    }
    if (result < 0 && result != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      impl_->last_error = mbedtls_error_message("TLS read", result);
    } else {
      impl_->last_error.clear();
    }
    return result;
  }
}

bool TlsConnection::write_all(const std::uint8_t* data, std::size_t size) {
  while (size > 0) {
    const int written = mbedtls_ssl_write(&impl_->ssl, data, size);
    if (retryable_tls_result(written)) {
      continue;
    }
    if (written <= 0) {
      impl_->last_error = mbedtls_error_message("TLS write", written);
      return false;
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
  impl_->last_error.clear();
  return true;
}

void TlsConnection::close_notify() {
  int result = 0;
  while (retryable_tls_result(result = mbedtls_ssl_close_notify(&impl_->ssl))) {
  }
  if (result < 0) {
    impl_->last_error = mbedtls_error_message("TLS close_notify", result);
  }
}

std::string TlsConnection::peer_fingerprint() const {
  const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&impl_->ssl);
  if (!peer || !peer->raw.p || peer->raw.len == 0) {
    return "";
  }
  const std::vector<std::uint8_t> der(peer->raw.p, peer->raw.p + peer->raw.len);
  return sha256_hex(der);
}

std::string TlsConnection::last_error() const {
  return impl_->last_error;
}

} // namespace localsend
