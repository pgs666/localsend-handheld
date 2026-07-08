#include "localsend/http.hpp"

#include "localsend/constants.hpp"
#include "localsend/safe_path.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#if LOCALSEND_PLATFORM_PSV
#include <pthread.h>
#endif
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace localsend {

class HttpStream {
public:
  virtual ~HttpStream() = default;
  virtual int read(char* data, std::size_t size) = 0;
  virtual bool write_all(const char* data, std::size_t size) = 0;
  virtual void close_notify() {}
};

namespace {

constexpr int kClientSocketTimeoutSeconds = 120;

int socket_send_flags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

#if LOCALSEND_PLATFORM_SWITCH
void set_fd_blocking(int fd, bool blocking) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return;
  }
  if (blocking) {
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  } else {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}
#endif

class TcpStream final : public HttpStream {
public:
  explicit TcpStream(int fd) : fd_(fd) {}
  ~TcpStream() override { close_fd(fd_); }

  int read(char* data, std::size_t size) override {
    return static_cast<int>(::recv(fd_, data, size, 0));
  }

  bool write_all(const char* data, std::size_t size) override {
    while (size > 0) {
      const ssize_t sent = ::send(fd_, data, size, socket_send_flags());
      if (sent <= 0) {
        return false;
      }
      data += sent;
      size -= static_cast<std::size_t>(sent);
    }
    return true;
  }

private:
  int fd_ = -1;
};

#if LOCALSEND_HAS_MBEDTLS
class TlsStream final : public HttpStream {
public:
  explicit TlsStream(TlsConnection connection) : connection_(std::move(connection)) {}

  int read(char* data, std::size_t size) override {
    return connection_.read(reinterpret_cast<std::uint8_t*>(data), size);
  }

  bool write_all(const char* data, std::size_t size) override {
    return connection_.write_all(reinterpret_cast<const std::uint8_t*>(data), size);
  }

  void close_notify() override {
    connection_.close_notify();
  }

private:
  TlsConnection connection_;
};
#endif

void set_socket_timeouts(int fd) {
  timeval timeout{};
  timeout.tv_sec = kClientSocketTimeoutSeconds;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

std::string status_text(int status) {
  switch (status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 409: return "Conflict";
  case 500: return "Internal Server Error";
  default: return "OK";
  }
}

std::string random_hex(std::size_t bytes) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes; ++i) {
    const auto value = static_cast<unsigned>(rng() & 0xFF);
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0x0F]);
  }
  return out;
}

std::string url_decode(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      const std::string hex = input.substr(i + 1, 2);
      char* end = nullptr;
      const long value = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        out.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i] == '+' ? ' ' : input[i]);
  }
  return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
  std::map<std::string, std::string> out;
  std::size_t begin = 0;
  while (begin <= query.size()) {
    const std::size_t amp = query.find('&', begin);
    const std::string part = query.substr(begin, amp == std::string::npos ? std::string::npos : amp - begin);
    if (!part.empty()) {
      const std::size_t eq = part.find('=');
      if (eq == std::string::npos) {
        out[url_decode(part)] = "";
      } else {
        out[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
      }
    }
    if (amp == std::string::npos) {
      break;
    }
    begin = amp + 1;
  }
  return out;
}

std::string with_query(const std::string& path, const std::map<std::string, std::string>& query) {
  if (query.empty()) {
    return path;
  }
  std::string out = path + "?";
  bool first = true;
  for (const auto& entry : query) {
    if (!first) {
      out += "&";
    }
    first = false;
    out += entry.first + "=" + entry.second;
  }
  return out;
}

bool recv_until_headers(HttpStream& stream, std::string& buffer) {
  char chunk[4096];
  while (buffer.find("\r\n\r\n") == std::string::npos) {
    const int got = stream.read(chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(got));
    if (buffer.size() > 1024 * 1024) {
      return false;
    }
  }
  return true;
}

std::map<std::string, std::string> parse_headers(std::istringstream& stream) {
  std::map<std::string, std::string> headers;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    headers[std::move(key)] = std::move(value);
  }
  return headers;
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::size_t content_length(const std::map<std::string, std::string>& headers) {
  for (const auto& entry : headers) {
    if (lower_ascii(entry.first) == "content-length") {
      return static_cast<std::size_t>(std::stoull(entry.second));
    }
  }
  return 0;
}

bool transfer_encoding_chunked(const std::map<std::string, std::string>& headers) {
  for (const auto& entry : headers) {
    if (lower_ascii(entry.first) == "transfer-encoding" && lower_ascii(entry.second).find("chunked") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool route_is(const HttpRequest& request, const char* path) {
  return request.path == path;
}

Device device_from_info_register(const InfoRegisterDto& dto, const std::string& ip, int fallback_port, bool fallback_https) {
  Device device;
  device.ip = ip;
  device.version = dto.version.empty() ? kProtocolVersion : dto.version;
  device.port = dto.port > 0 ? dto.port : fallback_port;
  device.https = dto.protocol == ProtocolType::Https || (dto.port <= 0 && fallback_https);
  device.fingerprint = dto.fingerprint;
  device.alias = dto.alias;
  device.device_model = dto.device_model;
  device.device_type = dto.device_type;
  device.download = dto.download;
  return device;
}

bool read_body(HttpStream& stream, std::string& buffer, std::size_t header_end, std::size_t length, std::string& body) {
  body = buffer.substr(header_end);
  while (body.size() < length) {
    char chunk[8192];
    const int got = stream.read(chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    body.append(chunk, static_cast<std::size_t>(got));
  }
  if (body.size() > length) {
    body.resize(length);
  }
  return true;
}

bool parse_request_headers(HttpStream& stream, HttpRequest& request, std::string& initial_body) {
  std::string buffer;
  if (!recv_until_headers(stream, buffer)) {
    return false;
  }
  const std::size_t header_end = buffer.find("\r\n\r\n") + 4;
  std::istringstream header_stream(buffer.substr(0, header_end));
  std::string target;
  std::string version;
  header_stream >> request.method >> target >> version;
  std::string discard;
  std::getline(header_stream, discard);
  request.headers = parse_headers(header_stream);

  const std::size_t query_pos = target.find('?');
  request.path = query_pos == std::string::npos ? target : target.substr(0, query_pos);
  if (query_pos != std::string::npos) {
    request.query = parse_query(target.substr(query_pos + 1));
  }

  request.content_length = content_length(request.headers);
  request.chunked = transfer_encoding_chunked(request.headers);
  initial_body = buffer.substr(header_end);
  if (!request.chunked && initial_body.size() > request.content_length) {
    initial_body.resize(request.content_length);
  }
  return true;
}

bool read_remaining_body(HttpStream& stream, const std::string& initial_body, std::size_t length, std::string& body) {
  body = initial_body;
  while (body.size() < length) {
    char chunk[kTransferBufferSize];
    const int got = stream.read(chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    body.append(chunk, static_cast<std::size_t>(got));
  }
  if (body.size() > length) {
    body.resize(length);
  }
  return true;
}

bool fill_stream_buffer(HttpStream& stream, std::string& buffer, std::size_t pos, std::size_t need) {
  char chunk[8192];
  while (buffer.size() - pos < need) {
    const int got = stream.read(chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(got));
  }
  return true;
}

bool read_chunked_body_to_stream(HttpStream& stream,
                                 const std::string& initial_body,
                                 std::ostream& out,
                                 std::size_t& written,
                                 const std::function<void(std::size_t)>& progress = nullptr) {
  std::string buffer = initial_body;
  std::size_t pos = 0;
  written = 0;

  while (true) {
    std::size_t line_end = buffer.find("\r\n", pos);
    while (line_end == std::string::npos) {
      if (!fill_stream_buffer(stream, buffer, pos, buffer.size() - pos + 1)) {
        return false;
      }
      line_end = buffer.find("\r\n", pos);
    }

    std::string size_text = buffer.substr(pos, line_end - pos);
    const std::size_t semicolon = size_text.find(';');
    if (semicolon != std::string::npos) {
      size_text.resize(semicolon);
    }
    const std::size_t chunk_size = static_cast<std::size_t>(std::strtoull(size_text.c_str(), nullptr, 16));
    pos = line_end + 2;

    if (chunk_size == 0) {
      while (true) {
        line_end = buffer.find("\r\n", pos);
        while (line_end == std::string::npos) {
          if (!fill_stream_buffer(stream, buffer, pos, buffer.size() - pos + 1)) {
            return false;
          }
          line_end = buffer.find("\r\n", pos);
        }
        if (line_end == pos) {
          return true;
        }
        pos = line_end + 2;
      }
    }

    if (!fill_stream_buffer(stream, buffer, pos, chunk_size + 2)) {
      return false;
    }
    out.write(buffer.data() + pos, static_cast<std::streamsize>(chunk_size));
    written += chunk_size;
    if (progress) {
      progress(written);
    }
    pos += chunk_size;
    if (pos + 1 >= buffer.size() || buffer[pos] != '\r' || buffer[pos + 1] != '\n') {
      return false;
    }
    pos += 2;

    if (pos > 64 * 1024) {
      buffer.erase(0, pos);
      pos = 0;
    }
  }
}

bool read_chunked_body_to_string(HttpStream& stream, const std::string& initial_body, std::string& body) {
  std::ostringstream out;
  std::size_t written = 0;
  if (!read_chunked_body_to_stream(stream, initial_body, out, written)) {
    return false;
  }
  body = out.str();
  return true;
}

bool response_version_is_legacy_v1(const std::string& body) {
  try {
    return info_from_json(Json::parse(body)).version == "1.0";
  } catch (const std::exception&) {
    return false;
  }
}

bool debug_send_enabled() {
  const char* value = std::getenv("LOCALSEND_DEBUG_SEND");
  return value && std::string(value) == "1";
}

void debug_send_line(const std::string& line) {
  if (debug_send_enabled()) {
    std::cerr << "[send] " << line << '\n';
  }
}

HttpResult request_raw(const std::string& host,
                       int port,
                       const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       const std::string& content_type,
                       bool use_tls = false,
                       const std::string& expected_fingerprint = "");

bool target_uses_v2_api(const Device& target) {
  if (target.version == "1.0") {
    return false;
  }
  if (!target.version.empty()) {
    return true;
  }

  const HttpResult v2_info = request_raw(target.ip, target.port, "GET", kRouteInfo, "", "", target.https, target.fingerprint);
  if (v2_info.status == 200) {
    return !response_version_is_legacy_v1(v2_info.body);
  }

  const HttpResult v1_info = request_raw(target.ip, target.port, "GET", kRouteInfoV1, "", "", target.https, target.fingerprint);
  if (v1_info.status == 200) {
    return !response_version_is_legacy_v1(v1_info.body);
  }

  return true;
}

bool info_matches_target_identity(const InfoDto& info, const Device& target) {
  if (!target.fingerprint.empty() && info.fingerprint == target.fingerprint) {
    return true;
  }
  if (!target.alias.empty() && info.alias == target.alias) {
    return true;
  }
  return target.fingerprint.empty() && target.alias.empty();
}

Device resolve_send_target(Device target) {
  if (!target.https) {
    return target;
  }

  const HttpResult http_info = request_raw(target.ip, target.port, "GET", kRouteInfo, "", "", false, "");
  if (http_info.status != 200) {
    return target;
  }

  try {
    const InfoDto info = info_from_json(Json::parse(http_info.body));
    if (!info_matches_target_identity(info, target)) {
      return target;
    }
    target.https = false;
    target.version = info.version.empty() ? target.version : info.version;
    target.fingerprint = info.fingerprint;
    target.alias = info.alias.empty() ? target.alias : info.alias;
    target.device_model = info.device_model.empty() ? target.device_model : info.device_model;
    target.device_type = info.device_type;
    target.download = info.download;
    debug_send_line("downgraded target transport to http after /info identity match");
  } catch (const std::exception&) {
    return target;
  }
  return target;
}

bool parse_response(HttpStream& stream, HttpResult& result) {
  std::string buffer;
  if (!recv_until_headers(stream, buffer)) {
    return false;
  }
  const std::size_t header_end = buffer.find("\r\n\r\n") + 4;
  std::istringstream header_stream(buffer.substr(0, header_end));
  std::string version;
  header_stream >> version >> result.status;
  std::string discard;
  std::getline(header_stream, discard);
  const auto headers = parse_headers(header_stream);
  if (transfer_encoding_chunked(headers)) {
    return read_chunked_body_to_string(stream, buffer.substr(header_end), result.body);
  }
  return read_body(stream, buffer, header_end, content_length(headers), result.body);
}

int connect_tcp(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
    return -1;
  }

  int fd = -1;
  for (addrinfo* item = result; item; item = item->ai_next) {
    fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
      break;
    }
    close_fd(fd);
    fd = -1;
  }

  ::freeaddrinfo(result);
  if (fd >= 0) {
    set_socket_timeouts(fd);
  }
  return fd;
}

std::unique_ptr<HttpStream> connect_http_stream(const std::string& host, int port, bool use_tls, const std::string& expected_fingerprint) {
  int fd = connect_tcp(host, port);
  if (fd < 0) {
    return nullptr;
  }
  if (!use_tls) {
    return std::make_unique<TcpStream>(fd);
  }

#if LOCALSEND_HAS_MBEDTLS
  try {
    auto tls = TlsConnection::client(fd);
    if (!tls.handshake()) {
      close_fd(fd);
      return nullptr;
    }
    const std::string actual_fingerprint = tls.peer_fingerprint();
    if (!expected_fingerprint.empty() && actual_fingerprint != expected_fingerprint) {
      tls.close_notify();
      return nullptr;
    }
    return std::make_unique<TlsStream>(std::move(tls));
  } catch (const std::exception&) {
    close_fd(fd);
    return nullptr;
  }
#else
  close_fd(fd);
  return nullptr;
#endif
}

HttpResult request_raw(const std::string& host,
                       int port,
                       const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       const std::string& content_type,
                       bool use_tls,
                       const std::string& expected_fingerprint) {
  HttpResult result;
  auto stream = connect_http_stream(host, port, use_tls, expected_fingerprint);
  if (!stream) {
    return result;
  }

  std::ostringstream request;
  request << method << ' ' << path << " HTTP/1.1\r\n"
          << "Host: " << host << ':' << port << "\r\n"
          << "Connection: close\r\n"
          << "Content-Length: " << body.size() << "\r\n";
  if (!content_type.empty()) {
    request << "Content-Type: " << content_type << "\r\n";
  }
  request << "\r\n" << body;

  const std::string text = request.str();
  if (stream->write_all(text.data(), text.size())) {
    parse_response(*stream, result);
  }
  stream->close_notify();
  return result;
}

HttpResult post_file_raw(const std::string& host,
                         int port,
                         const std::string& path,
                         std::ifstream& file,
                         std::uint64_t size,
                         const std::string& content_type,
                         bool use_tls = false,
                         const std::string& expected_fingerprint = "",
                         const std::function<void(std::uint64_t)>& progress = nullptr,
                         const std::function<bool()>& should_cancel = nullptr) {
  HttpResult result;
  auto stream = connect_http_stream(host, port, use_tls, expected_fingerprint);
  if (!stream) {
    return result;
  }

  std::ostringstream request;
  request << "POST " << path << " HTTP/1.1\r\n"
          << "Host: " << host << ':' << port << "\r\n"
          << "Connection: close\r\n"
          << "Content-Length: " << size << "\r\n"
          << "Content-Type: " << content_type << "\r\n\r\n";

  const std::string headers = request.str();
  bool ok = stream->write_all(headers.data(), headers.size());
  std::vector<char> buffer(kTransferBufferSize);
  std::uint64_t uploaded = 0;
  while (ok && file) {
    if (should_cancel && should_cancel()) {
      result.body = "cancelled";
      ok = false;
      break;
    }
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = file.gcount();
    if (got > 0) {
      ok = stream->write_all(buffer.data(), static_cast<std::size_t>(got));
      uploaded += static_cast<std::uint64_t>(got);
      if (ok && progress) {
        progress(uploaded);
      }
    }
  }
  if (ok && !(should_cancel && should_cancel())) {
    parse_response(*stream, result);
  } else if (should_cancel && should_cancel()) {
    result.body = "cancelled";
  }
  stream->close_notify();
  return result;
}

HttpResult post_chunked_raw(const std::string& host,
                            int port,
                            const std::string& path,
                            const std::vector<std::string>& chunks,
                            const std::string& content_type,
                            bool use_tls = false,
                            const std::string& expected_fingerprint = "") {
  HttpResult result;
  auto stream = connect_http_stream(host, port, use_tls, expected_fingerprint);
  if (!stream) {
    return result;
  }

  std::ostringstream request;
  request << "POST " << path << " HTTP/1.1\r\n"
          << "Host: " << host << ':' << port << "\r\n"
          << "Connection: close\r\n"
          << "Transfer-Encoding: chunked\r\n"
          << "Content-Type: " << content_type << "\r\n\r\n";

  const std::string headers = request.str();
  bool ok = stream->write_all(headers.data(), headers.size());
  for (const auto& chunk : chunks) {
    if (!ok) {
      break;
    }
    std::ostringstream prefix;
    prefix << std::hex << chunk.size() << "\r\n";
    const std::string header = prefix.str();
    ok = stream->write_all(header.data(), header.size()) &&
         stream->write_all(chunk.data(), chunk.size()) &&
         stream->write_all("\r\n", 2);
  }
  ok = ok && stream->write_all("0\r\n\r\n", 5);
  if (ok) {
    parse_response(*stream, result);
  }
  stream->close_notify();
  return result;
}

HttpResponse json_response(int status, Json json) {
  HttpResponse response;
  response.status = status;
  response.body = json.dump();
  return response;
}

HttpResponse text_response(int status, const std::string& text) {
  HttpResponse response;
  response.status = status;
  response.content_type = "text/plain";
  response.body = text;
  return response;
}

} // namespace

HttpResult http_get(const std::string& host, int port, const std::string& path) {
  return request_raw(host, port, "GET", path, "", "");
}

HttpResult https_get(const std::string& host, int port, const std::string& path, const std::string& expected_fingerprint) {
  return request_raw(host, port, "GET", path, "", "", true, expected_fingerprint);
}

HttpResult http_post(const std::string& host,
                     int port,
                     const std::string& path,
                     const std::string& body,
                     const std::string& content_type) {
  return request_raw(host, port, "POST", path, body, content_type);
}

HttpResult https_post(const std::string& host,
                      int port,
                      const std::string& path,
                      const std::string& body,
                      const std::string& content_type,
                      const std::string& expected_fingerprint) {
  return request_raw(host, port, "POST", path, body, content_type, true, expected_fingerprint);
}

HttpResult http_post_chunked(const std::string& host,
                             int port,
                             const std::string& path,
                             const std::vector<std::string>& chunks,
                             const std::string& content_type) {
  return post_chunked_raw(host, port, path, chunks, content_type);
}

LocalSendServer::LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox)
    : self_(std::move(self)), inbox_(std::move(inbox)) {}

LocalSendServer::LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox, TransferStore* transfers)
    : self_(std::move(self)), inbox_(std::move(inbox)), transfers_(transfers) {}

LocalSendServer::LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox, TlsCredentials tls_credentials)
    : self_(std::move(self)), inbox_(std::move(inbox)), tls_credentials_(std::move(tls_credentials)) {
  self_.protocol = ProtocolType::Https;
}

LocalSendServer::LocalSendServer(InfoRegisterDto self,
                                 std::filesystem::path inbox,
                                 TlsCredentials tls_credentials,
                                 TransferStore* transfers)
    : self_(std::move(self)),
      inbox_(std::move(inbox)),
      tls_credentials_(std::move(tls_credentials)),
      transfers_(transfers) {
  self_.protocol = ProtocolType::Https;
}

LocalSendServer::~LocalSendServer() {
  stop();
}

bool LocalSendServer::start(int requested_port) {
  if (running_) {
    return true;
  }

  try {
    std::filesystem::create_directories(inbox_);
  } catch (const std::exception&) {
    return false;
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return false;
  }

  int enabled = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(requested_port));
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 8) != 0) {
    close_fd(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    port_ = ntohs(addr.sin_port);
    self_.port = port_;
  }

  running_ = true;
#if LOCALSEND_PLATFORM_PSV
  if (::pthread_create(&accept_thread_, nullptr, &LocalSendServer::accept_thread_entry, this) != 0) {
    running_ = false;
    close_fd(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  accept_thread_started_ = true;
#elif LOCALSEND_PLATFORM_SWITCH
  set_fd_blocking(listen_fd_, false);
#else
  accept_thread_ = std::thread(&LocalSendServer::accept_loop, this);
#endif
  return true;
}

void LocalSendServer::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  ::shutdown(listen_fd_, SHUT_RDWR);
  close_fd(listen_fd_);
  listen_fd_ = -1;
#if LOCALSEND_PLATFORM_PSV
  if (accept_thread_started_) {
    ::pthread_join(accept_thread_, nullptr);
    accept_thread_started_ = false;
  }
#else
#if !LOCALSEND_PLATFORM_SWITCH
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
#endif
#endif
}

void LocalSendServer::poll_once() {
#if LOCALSEND_PLATFORM_SWITCH
  if (!running_ || listen_fd_ < 0) {
    return;
  }

  sockaddr_in sender{};
  socklen_t sender_len = sizeof(sender);
  const int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&sender), &sender_len);
  if (client >= 0) {
    std::string remote_ip;
    char ip[INET_ADDRSTRLEN] = {};
    if (sender.sin_family == AF_INET && ::inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip))) {
      remote_ip = ip;
    }
    set_fd_blocking(client, true);
    handle_client(client, std::move(remote_ip));
  }
#endif
}

void LocalSendServer::accept_loop() {
  while (running_) {
    sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);
    const int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&sender), &sender_len);
    if (client < 0) {
      if (running_) {
        continue;
      }
      break;
    }
    std::string remote_ip;
    char ip[INET_ADDRSTRLEN] = {};
    if (sender.sin_family == AF_INET && ::inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip))) {
      remote_ip = ip;
    }
#if LOCALSEND_PLATFORM_PSV
    handle_client(client, std::move(remote_ip));
#else
    std::thread(&LocalSendServer::handle_client, this, client, std::move(remote_ip)).detach();
#endif
  }
}

#if LOCALSEND_PLATFORM_PSV
void* LocalSendServer::accept_thread_entry(void* arg) {
  static_cast<LocalSendServer*>(arg)->accept_loop();
  return nullptr;
}
#endif

void LocalSendServer::set_register_callback(std::function<void(Device)> callback) {
  std::lock_guard<std::mutex> lock(register_callback_mutex_);
  register_callback_ = std::move(callback);
}

void LocalSendServer::handle_client(int client_fd, std::string remote_ip) {
  std::unique_ptr<HttpStream> stream;
  if (tls_credentials_) {
#if LOCALSEND_HAS_MBEDTLS
    try {
      auto tls = TlsConnection::server(client_fd, *tls_credentials_);
      if (!tls.handshake()) {
        close_fd(client_fd);
        return;
      }
      stream = std::make_unique<TlsStream>(std::move(tls));
    } catch (const std::exception&) {
      close_fd(client_fd);
      return;
    }
#else
    close_fd(client_fd);
    return;
#endif
  } else {
    stream = std::make_unique<TcpStream>(client_fd);
  }

  HttpRequest request;
  std::string initial_body;
  HttpResponse response;
  if (!parse_request_headers(*stream, request, initial_body)) {
    response = text_response(400, "bad request");
  } else if (request.method == "POST" && (route_is(request, kRouteUpload) || route_is(request, kRouteUploadV1))) {
    response = handle_upload(*stream, request, initial_body, route_is(request, kRouteUpload));
  } else if (!read_remaining_body(*stream, initial_body, request.content_length, request.body)) {
    response = text_response(400, "bad request body");
  } else {
    response = route(request, remote_ip);
  }

  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\n"
      << "Content-Type: " << response.content_type << "\r\n"
      << "Content-Length: " << response.body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << response.body;
  const std::string text = out.str();
  stream->write_all(text.data(), text.size());
  stream->close_notify();
}

HttpResponse LocalSendServer::route(const HttpRequest& request, const std::string& remote_ip) {
  if (request.method == "GET" && (route_is(request, kRouteInfo) || route_is(request, kRouteInfoV1))) {
    return handle_info(request);
  }
  if (request.method == "POST" && (route_is(request, kRouteRegister) || route_is(request, kRouteRegisterV1))) {
    return handle_register(request, remote_ip);
  }
  if (request.method == "POST" && (route_is(request, kRoutePrepareUpload) || route_is(request, kRoutePrepareUploadV1))) {
    return handle_prepare_upload(request, route_is(request, kRoutePrepareUpload));
  }
  if (request.method == "POST" && (route_is(request, kRouteUpload) || route_is(request, kRouteUploadV1))) {
    return text_response(400, "upload route requires streaming handler");
  }
  if (request.method == "POST" && (route_is(request, kRouteCancel) || route_is(request, kRouteCancelV1))) {
    return handle_cancel(request);
  }
  return text_response(404, "not found");
}

HttpResponse LocalSendServer::handle_info(const HttpRequest& request) const {
  const auto fingerprint = request.query.find("fingerprint");
  if (fingerprint != request.query.end() && !self_.fingerprint.empty() && fingerprint->second == self_.fingerprint) {
    return text_response(412, "Self-discovered");
  }
  return json_response(200, to_json(self_));
}

HttpResponse LocalSendServer::handle_register(const HttpRequest& request, const std::string& remote_ip) {
  InfoRegisterDto dto;
  try {
    dto = info_register_from_json(Json::parse(request.body));
  } catch (const std::exception&) {
    return text_response(400, "Request body malformed");
  }

  if (!self_.fingerprint.empty() && dto.fingerprint == self_.fingerprint) {
    return text_response(412, "Self-discovered");
  }

  std::function<void(Device)> callback;
  {
    std::lock_guard<std::mutex> lock(register_callback_mutex_);
    callback = register_callback_;
  }
  if (callback && !remote_ip.empty()) {
    callback(device_from_info_register(dto, remote_ip, self_.port, self_.protocol == ProtocolType::Https));
  }

  return json_response(200, to_json(static_cast<const InfoDto&>(self_)));
}

HttpResponse LocalSendServer::handle_prepare_upload(const HttpRequest& request, bool v2) {
  PrepareUploadRequestDto dto;
  try {
    dto = prepare_upload_request_from_json(Json::parse(request.body));
  } catch (const std::exception& e) {
    return text_response(400, e.what());
  }

  Session session;
  session.session_id = random_hex(8);
  PrepareUploadResponseDto response;
  response.session_id = session.session_id;

  for (const auto& entry : dto.files) {
    PendingFile pending;
    pending.dto = entry.second;
    pending.token = random_hex(12);
    pending.destination = unique_destination_path(inbox_, pending.dto.file_name);
    if (transfers_) {
      pending.transfer_id = transfers_->add(TransferDirection::Receive,
                                            pending.dto.file_name,
                                            pending.dto.size,
                                            dto.info.alias,
                                            "");
      transfers_->set_status(pending.transfer_id, TransferStatus::Preparing);
    }
    session.files.emplace(pending.dto.id, pending);
    response.files.emplace(pending.dto.id, pending.token);
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.emplace(session.session_id, std::move(session));
  }

  if (v2) {
    return json_response(200, to_json(response));
  }

  Json files = Json::object();
  for (const auto& entry : response.files) {
    files[entry.first] = entry.second;
  }
  return json_response(200, files);
}

HttpResponse LocalSendServer::handle_upload(HttpStream& stream, const HttpRequest& request, const std::string& initial_body, bool v2) {
  const auto session_it = request.query.find("sessionId");
  const auto file_it = request.query.find("fileId");
  const auto token_it = request.query.find("token");
  if ((v2 && session_it == request.query.end()) || file_it == request.query.end() || token_it == request.query.end()) {
    return text_response(400, "missing upload query");
  }

  std::filesystem::path destination;
  std::uint64_t expected_size = 0;
  std::uint64_t transfer_id = 0;
  std::string session_id;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session = v2 ? sessions_.find(session_it->second) : sessions_.end();
    if (!v2 && sessions_.size() == 1) {
      session = sessions_.begin();
    }
    if (session == sessions_.end() || session->second.cancelled) {
      return text_response(404, "session not found");
    }
    auto file = session->second.files.find(file_it->second);
    if (file == session->second.files.end()) {
      return text_response(404, "file not found");
    }
    if (file->second.token != token_it->second) {
      return text_response(403, "invalid token");
    }
    if (!request.chunked && request.content_length != file->second.dto.size) {
      return text_response(400, "file size mismatch");
    }
    destination = file->second.destination;
    expected_size = file->second.dto.size;
    transfer_id = file->second.transfer_id;
    session_id = session->first;
  }

  std::ofstream out(destination, std::ios::binary);
  if (!out) {
    if (transfers_ && transfer_id != 0) {
      transfers_->fail(transfer_id, "failed to open destination");
    }
    return text_response(500, "failed to open destination");
  }

  auto failed_upload = [&](int status, const std::string& message) {
    out.close();
    std::filesystem::remove(destination);
    if (transfers_ && transfer_id != 0) {
      transfers_->fail(transfer_id, message);
    }
    return text_response(status, message);
  };

  std::size_t written = 0;
  auto update_progress = [&](std::size_t bytes) {
    if (transfers_ && transfer_id != 0) {
      transfers_->set_progress(transfer_id, static_cast<std::uint64_t>(bytes));
    }
  };
  if (request.chunked) {
    if (!read_chunked_body_to_stream(stream, initial_body, out, written, update_progress)) {
      return failed_upload(400, "incomplete upload");
    }
  } else {
    if (!initial_body.empty()) {
      out.write(initial_body.data(), static_cast<std::streamsize>(initial_body.size()));
      written += initial_body.size();
      update_progress(written);
    }

    std::vector<char> buffer(kTransferBufferSize);
    while (written < request.content_length) {
      const std::size_t remaining = request.content_length - written;
      const std::size_t chunk_size = std::min(buffer.size(), remaining);
      const int got = stream.read(buffer.data(), chunk_size);
      if (got <= 0) {
        return failed_upload(400, "incomplete upload");
      }
      out.write(buffer.data(), static_cast<std::streamsize>(got));
      written += static_cast<std::size_t>(got);
      update_progress(written);
    }
  }

  if (written != request.content_length && !request.chunked) {
    return failed_upload(400, "incomplete upload");
  }
  if (request.chunked && written != expected_size) {
    return failed_upload(400, "file size mismatch");
  }
  if (!out) {
    return failed_upload(500, "failed to write destination");
  }
  out.close();
  if (transfers_ && transfer_id != 0) {
    transfers_->set_status(transfer_id, TransferStatus::Completed);
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session = sessions_.find(session_id);
    if (session != sessions_.end()) {
      auto file = session->second.files.find(file_it->second);
      if (file != session->second.files.end()) {
        file->second.complete = true;
      }
    }
  }

  return json_response(200, Json::object());
}

HttpResponse LocalSendServer::handle_cancel(const HttpRequest& request) {
  const auto session_it = request.query.find("sessionId");
  if (session_it == request.query.end()) {
    return text_response(400, "missing sessionId");
  }

  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto session = sessions_.find(session_it->second);
  if (session == sessions_.end()) {
    return text_response(404, "session not found");
  }
  session->second.cancelled = true;
  if (transfers_) {
    for (const auto& file : session->second.files) {
      if (file.second.transfer_id != 0) {
        transfers_->cancel(file.second.transfer_id);
      }
    }
  }
  return text_response(200, "ok");
}

bool send_files_http(const Device& target,
                     const std::vector<std::filesystem::path>& file_paths,
                     const InfoRegisterDto& self,
                     TransferStore* transfers) {
  return send_files_http_detailed(target, file_paths, self, transfers).ok;
}

SendFilesResult send_files_http_detailed(const Device& target,
                                         const std::vector<std::filesystem::path>& file_paths,
                                         const InfoRegisterDto& self,
                                         TransferStore* transfers,
                                         SendFilesControl* control) {
  auto result_error = [](std::string message) {
    SendFilesResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
  };
  auto result_cancelled = []() {
    SendFilesResult result;
    result.ok = false;
    result.cancelled = true;
    result.error = "send cancelled";
    return result;
  };
  auto result_ok = []() {
    SendFilesResult result;
    result.ok = true;
    return result;
  };
  auto compact_body = [](std::string body) {
    constexpr std::size_t kMaxBody = 120;
    for (char& c : body) {
      if (c == '\r' || c == '\n' || c == '\t') {
        c = ' ';
      }
    }
    if (body.size() > kMaxBody) {
      body.resize(kMaxBody);
      body += "...";
    }
    return body;
  };
  auto http_failure_detail = [&compact_body](const HttpResult& response) {
    if (response.status == 0 && response.body.empty()) {
      return std::string("connection failed or no response");
    }
    return compact_body(response.body);
  };
  auto fail_transfers = [transfers](const std::vector<std::uint64_t>& transfer_ids, const std::string& message) {
    if (!transfers) {
      return;
    }
    for (const std::uint64_t transfer_id : transfer_ids) {
      if (transfer_id != 0) {
        transfers->fail(transfer_id, message);
      }
    }
  };
  auto cancel_requested = [control]() {
    return control && control->cancel_requested.load();
  };
  auto cancel_transfers = [transfers](const std::vector<std::uint64_t>& transfer_ids) {
    if (!transfers) {
      return;
    }
    for (const std::uint64_t transfer_id : transfer_ids) {
      if (transfer_id != 0) {
        transfers->cancel(transfer_id);
      }
    }
  };
  if (file_paths.empty()) {
    return result_error("no files selected");
  }

  const Device send_target = resolve_send_target(target);
  auto send_cancel = [&send_target](const std::string& session_id, bool v2) {
    if (!v2 || session_id.empty()) {
      return;
    }
    const std::string path = std::string(kRouteCancel) + "?sessionId=" + session_id;
    static_cast<void>(request_raw(send_target.ip,
                                  send_target.port,
                                  "POST",
                                  path,
                                  "",
                                  "application/json",
                                  send_target.https,
                                  send_target.fingerprint));
  };

  std::vector<FileDto> files;
  std::vector<std::uint64_t> transfer_ids;
  files.reserve(file_paths.size());
  transfer_ids.reserve(file_paths.size());
  for (std::size_t i = 0; i < file_paths.size(); ++i) {
    std::ifstream probe(file_paths[i], std::ios::binary);
    if (!probe) {
      return result_error("source file is not readable: " + file_paths[i].filename().string());
    }

    FileDto file;
    file.id = std::to_string(i);
    file.file_name = file_paths[i].filename().string();
    file.size = static_cast<std::uint64_t>(std::filesystem::file_size(file_paths[i]));
    file.file_type = mime_from_filename(file.file_name);
    if (transfers) {
      const std::uint64_t transfer_id = transfers->add(TransferDirection::Send,
                                                       file.file_name,
                                                       file.size,
                                                       send_target.alias,
                                                       send_target.ip);
      transfers->set_status(transfer_id, TransferStatus::Preparing);
      transfer_ids.push_back(transfer_id);
    } else {
      transfer_ids.push_back(0);
    }
    files.push_back(std::move(file));
  }

  if (cancel_requested()) {
    cancel_transfers(transfer_ids);
    return result_cancelled();
  }

  PrepareUploadRequestDto request;
  request.info = self;
  for (const auto& file : files) {
    request.files.emplace(file.id, file);
  }

  const bool v2 = target_uses_v2_api(send_target);
  debug_send_line(std::string("target api=") + (v2 ? "v2" : "v1"));
  const HttpResult prepared = request_raw(send_target.ip,
                                          send_target.port,
                                          "POST",
                                          v2 ? kRoutePrepareUpload : kRoutePrepareUploadV1,
                                          to_json(request).dump(),
                                          "application/json",
                                          send_target.https,
                                          send_target.fingerprint);
  debug_send_line("prepare status=" + std::to_string(prepared.status) + " body=" + prepared.body);
  if (cancel_requested()) {
    cancel_transfers(transfer_ids);
    return result_cancelled();
  }
  if (prepared.status == 204) {
    if (transfers) {
      for (const std::uint64_t transfer_id : transfer_ids) {
        if (transfer_id != 0) {
          transfers->set_status(transfer_id, TransferStatus::Completed);
        }
      }
    }
    return result_ok();
  }
  if (prepared.status != 200) {
    const std::string message = "prepare-upload failed status=" + std::to_string(prepared.status) +
                                " body=" + http_failure_detail(prepared);
    fail_transfers(transfer_ids, message);
    return result_error(message);
  }

  PrepareUploadResponseDto response;
  try {
    const Json body = Json::parse(prepared.body);
    if (v2) {
      response = prepare_upload_response_from_json(body);
    } else {
      response.session_id = "";
      for (const auto& entry : body.as_object()) {
        response.files.emplace(entry.first, entry.second.as_string());
      }
    }
  } catch (const std::exception&) {
    const std::string message = "invalid prepare-upload response: " + compact_body(prepared.body);
    fail_transfers(transfer_ids, message);
    return result_error(message);
  }

  if (v2 && response.session_id.empty()) {
    const std::string message = "prepare-upload response missing sessionId";
    fail_transfers(transfer_ids, message);
    return result_error(message);
  }

  bool all_uploaded = true;
  std::string first_error;
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (cancel_requested()) {
      send_cancel(response.session_id, v2);
      cancel_transfers(transfer_ids);
      return result_cancelled();
    }
    const FileDto& file = files[i];
    const std::uint64_t transfer_id = transfer_ids[i];
    const auto token = response.files.find(file.id);
    if (token == response.files.end()) {
      if (transfers && transfer_id != 0) {
        transfers->set_status(transfer_id, TransferStatus::Completed);
      }
      continue;
    }

    std::ifstream in(file_paths[i], std::ios::binary);
    if (!in) {
      if (transfers && transfer_id != 0) {
        transfers->fail(transfer_id, "failed to open source file");
      }
      return result_error("failed to open source file: " + file.file_name);
    }

    std::map<std::string, std::string> query = {
        {"fileId", file.id},
        {"token", token->second},
    };
    if (v2) {
      query.emplace("sessionId", response.session_id);
    }
    const std::string path = with_query(v2 ? kRouteUpload : kRouteUploadV1, query);
    debug_send_line("upload path=" + path + " size=" + std::to_string(file.size));
    const HttpResult uploaded = post_file_raw(send_target.ip,
                                              send_target.port,
                                              path,
                                              in,
                                              file.size,
                                              file.file_type,
                                              send_target.https,
                                              send_target.fingerprint,
                                              [transfers, transfer_id](std::uint64_t bytes) {
                                                if (transfers && transfer_id != 0) {
                                                  transfers->set_progress(transfer_id, bytes);
                                                }
                                              },
                                              cancel_requested);
    debug_send_line("upload status=" + std::to_string(uploaded.status) + " body=" + uploaded.body);
    if (cancel_requested()) {
      send_cancel(response.session_id, v2);
      cancel_transfers(transfer_ids);
      return result_cancelled();
    }
    if (uploaded.status != 200) {
      const std::string message = "upload failed file=" + file.file_name +
                                  " status=" + std::to_string(uploaded.status) +
                                  " body=" + http_failure_detail(uploaded);
      if (transfers && transfer_id != 0) {
        transfers->fail(transfer_id, message);
      }
      if (first_error.empty()) {
        first_error = message;
      }
      all_uploaded = false;
    } else if (transfers && transfer_id != 0) {
      transfers->set_status(transfer_id, TransferStatus::Completed);
    }
  }

  if (!all_uploaded) {
    return result_error(first_error.empty() ? "one or more uploads failed" : first_error);
  }
  return result_ok();
}

bool send_files_http(const Device& target, const std::vector<std::filesystem::path>& file_paths, const InfoRegisterDto& self) {
  return send_files_http(target, file_paths, self, nullptr);
}

bool send_single_file_http(const Device& target, const std::filesystem::path& file_path, const InfoRegisterDto& self) {
  return send_files_http(target, {file_path}, self);
}

} // namespace localsend
