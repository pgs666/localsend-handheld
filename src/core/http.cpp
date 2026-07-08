#include "localsend/http.hpp"

#include "localsend/constants.hpp"
#include "localsend/safe_path.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace localsend {
namespace {

constexpr int kClientSocketTimeoutSeconds = 120;

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

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

bool send_all(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t sent = ::send(fd, data, size, 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

bool recv_until_headers(int fd, std::string& buffer) {
  char chunk[4096];
  while (buffer.find("\r\n\r\n") == std::string::npos) {
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
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

bool read_body(int fd, std::string& buffer, std::size_t header_end, std::size_t length, std::string& body) {
  body = buffer.substr(header_end);
  while (body.size() < length) {
    char chunk[8192];
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
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

bool parse_request_headers(int fd, HttpRequest& request, std::string& initial_body) {
  std::string buffer;
  if (!recv_until_headers(fd, buffer)) {
    return false;
  }
  const std::size_t header_end = buffer.find("\r\n\r\n") + 4;
  std::istringstream stream(buffer.substr(0, header_end));
  std::string target;
  std::string version;
  stream >> request.method >> target >> version;
  std::string discard;
  std::getline(stream, discard);
  request.headers = parse_headers(stream);

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

bool read_remaining_body(int fd, const std::string& initial_body, std::size_t length, std::string& body) {
  body = initial_body;
  while (body.size() < length) {
    char chunk[kTransferBufferSize];
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
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

bool fill_stream_buffer(int fd, std::string& buffer, std::size_t pos, std::size_t need) {
  char chunk[8192];
  while (buffer.size() - pos < need) {
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
    if (got <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(got));
  }
  return true;
}

bool read_chunked_body_to_stream(int fd, const std::string& initial_body, std::ostream& out, std::size_t& written) {
  std::string buffer = initial_body;
  std::size_t pos = 0;
  written = 0;

  while (true) {
    std::size_t line_end = buffer.find("\r\n", pos);
    while (line_end == std::string::npos) {
      if (!fill_stream_buffer(fd, buffer, pos, buffer.size() - pos + 1)) {
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
          if (!fill_stream_buffer(fd, buffer, pos, buffer.size() - pos + 1)) {
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

    if (!fill_stream_buffer(fd, buffer, pos, chunk_size + 2)) {
      return false;
    }
    out.write(buffer.data() + pos, static_cast<std::streamsize>(chunk_size));
    written += chunk_size;
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

bool read_chunked_body_to_string(int fd, const std::string& initial_body, std::string& body) {
  std::ostringstream out;
  std::size_t written = 0;
  if (!read_chunked_body_to_stream(fd, initial_body, out, written)) {
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

bool target_uses_v2_api(const Device& target) {
  if (target.version == "1.0") {
    return false;
  }
  if (!target.version.empty()) {
    return true;
  }

  const HttpResult v2_info = http_get(target.ip, target.port, kRouteInfo);
  if (v2_info.status == 200) {
    return !response_version_is_legacy_v1(v2_info.body);
  }

  const HttpResult v1_info = http_get(target.ip, target.port, kRouteInfoV1);
  if (v1_info.status == 200) {
    return !response_version_is_legacy_v1(v1_info.body);
  }

  return true;
}

bool parse_response(int fd, HttpResult& result) {
  std::string buffer;
  if (!recv_until_headers(fd, buffer)) {
    return false;
  }
  const std::size_t header_end = buffer.find("\r\n\r\n") + 4;
  std::istringstream stream(buffer.substr(0, header_end));
  std::string version;
  stream >> version >> result.status;
  std::string discard;
  std::getline(stream, discard);
  const auto headers = parse_headers(stream);
  if (transfer_encoding_chunked(headers)) {
    return read_chunked_body_to_string(fd, buffer.substr(header_end), result.body);
  }
  return read_body(fd, buffer, header_end, content_length(headers), result.body);
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

HttpResult request_raw(const std::string& host,
                       int port,
                       const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       const std::string& content_type) {
  HttpResult result;
  const int fd = connect_tcp(host, port);
  if (fd < 0) {
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
  if (send_all(fd, text.data(), text.size())) {
    parse_response(fd, result);
  }
  close_fd(fd);
  return result;
}

HttpResult post_file_raw(const std::string& host,
                         int port,
                         const std::string& path,
                         std::ifstream& file,
                         std::uint64_t size,
                         const std::string& content_type) {
  HttpResult result;
  const int fd = connect_tcp(host, port);
  if (fd < 0) {
    return result;
  }

  std::ostringstream request;
  request << "POST " << path << " HTTP/1.1\r\n"
          << "Host: " << host << ':' << port << "\r\n"
          << "Connection: close\r\n"
          << "Content-Length: " << size << "\r\n"
          << "Content-Type: " << content_type << "\r\n\r\n";

  const std::string headers = request.str();
  bool ok = send_all(fd, headers.data(), headers.size());
  std::vector<char> buffer(kTransferBufferSize);
  while (ok && file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = file.gcount();
    if (got > 0) {
      ok = send_all(fd, buffer.data(), static_cast<std::size_t>(got));
    }
  }
  if (ok) {
    parse_response(fd, result);
  }
  close_fd(fd);
  return result;
}

HttpResult post_chunked_raw(const std::string& host,
                            int port,
                            const std::string& path,
                            const std::vector<std::string>& chunks,
                            const std::string& content_type) {
  HttpResult result;
  const int fd = connect_tcp(host, port);
  if (fd < 0) {
    return result;
  }

  std::ostringstream request;
  request << "POST " << path << " HTTP/1.1\r\n"
          << "Host: " << host << ':' << port << "\r\n"
          << "Connection: close\r\n"
          << "Transfer-Encoding: chunked\r\n"
          << "Content-Type: " << content_type << "\r\n\r\n";

  const std::string headers = request.str();
  bool ok = send_all(fd, headers.data(), headers.size());
  for (const auto& chunk : chunks) {
    if (!ok) {
      break;
    }
    std::ostringstream prefix;
    prefix << std::hex << chunk.size() << "\r\n";
    const std::string header = prefix.str();
    ok = send_all(fd, header.data(), header.size()) &&
         send_all(fd, chunk.data(), chunk.size()) &&
         send_all(fd, "\r\n", 2);
  }
  ok = ok && send_all(fd, "0\r\n\r\n", 5);
  if (ok) {
    parse_response(fd, result);
  }
  close_fd(fd);
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

HttpResult http_post(const std::string& host,
                     int port,
                     const std::string& path,
                     const std::string& body,
                     const std::string& content_type) {
  return request_raw(host, port, "POST", path, body, content_type);
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

LocalSendServer::~LocalSendServer() {
  stop();
}

bool LocalSendServer::start(int requested_port) {
  if (running_) {
    return true;
  }

  std::filesystem::create_directories(inbox_);
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
  accept_thread_ = std::thread(&LocalSendServer::accept_loop, this);
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
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
}

void LocalSendServer::accept_loop() {
  while (running_) {
    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      if (running_) {
        continue;
      }
      break;
    }
    std::thread(&LocalSendServer::handle_client, this, client).detach();
  }
}

void LocalSendServer::handle_client(int client_fd) {
  HttpRequest request;
  std::string initial_body;
  HttpResponse response;
  if (!parse_request_headers(client_fd, request, initial_body)) {
    response = text_response(400, "bad request");
  } else if (request.method == "POST" && (route_is(request, kRouteUpload) || route_is(request, kRouteUploadV1))) {
    response = handle_upload(client_fd, request, initial_body, route_is(request, kRouteUpload));
  } else if (!read_remaining_body(client_fd, initial_body, request.content_length, request.body)) {
    response = text_response(400, "bad request body");
  } else {
    response = route(request);
  }

  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\n"
      << "Content-Type: " << response.content_type << "\r\n"
      << "Content-Length: " << response.body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << response.body;
  const std::string text = out.str();
  send_all(client_fd, text.data(), text.size());
  close_fd(client_fd);
}

HttpResponse LocalSendServer::route(const HttpRequest& request) {
  if (request.method == "GET" && (route_is(request, kRouteInfo) || route_is(request, kRouteInfoV1))) {
    return handle_info();
  }
  if (request.method == "POST" && (route_is(request, kRouteRegister) || route_is(request, kRouteRegisterV1))) {
    return handle_info();
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

HttpResponse LocalSendServer::handle_info() const {
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

HttpResponse LocalSendServer::handle_upload(int client_fd, const HttpRequest& request, const std::string& initial_body, bool v2) {
  const auto session_it = request.query.find("sessionId");
  const auto file_it = request.query.find("fileId");
  const auto token_it = request.query.find("token");
  if ((v2 && session_it == request.query.end()) || file_it == request.query.end() || token_it == request.query.end()) {
    return text_response(400, "missing upload query");
  }

  std::filesystem::path destination;
  std::uint64_t expected_size = 0;
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
  }

  std::ofstream out(destination, std::ios::binary);
  if (!out) {
    return text_response(500, "failed to open destination");
  }

  std::size_t written = 0;
  if (request.chunked) {
    if (!read_chunked_body_to_stream(client_fd, initial_body, out, written)) {
      return text_response(400, "incomplete upload");
    }
  } else {
    if (!initial_body.empty()) {
      out.write(initial_body.data(), static_cast<std::streamsize>(initial_body.size()));
      written += initial_body.size();
    }

    std::vector<char> buffer(kTransferBufferSize);
    while (written < request.content_length) {
      const std::size_t remaining = request.content_length - written;
      const std::size_t chunk_size = std::min(buffer.size(), remaining);
      const ssize_t got = ::recv(client_fd, buffer.data(), chunk_size, 0);
      if (got <= 0) {
        return text_response(400, "incomplete upload");
      }
      out.write(buffer.data(), static_cast<std::streamsize>(got));
      written += static_cast<std::size_t>(got);
    }
  }

  if (written != request.content_length && !request.chunked) {
    return text_response(400, "incomplete upload");
  }
  if (request.chunked && written != expected_size) {
    return text_response(400, "file size mismatch");
  }
  if (!out) {
    return text_response(500, "failed to write destination");
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session = sessions_.find(session_it->second);
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
  return text_response(200, "ok");
}

bool send_files_http(const Device& target, const std::vector<std::filesystem::path>& file_paths, const InfoRegisterDto& self) {
  if (target.https) {
    return false;
  }
  if (file_paths.empty()) {
    return false;
  }

  std::vector<FileDto> files;
  files.reserve(file_paths.size());
  for (std::size_t i = 0; i < file_paths.size(); ++i) {
    std::ifstream probe(file_paths[i], std::ios::binary);
    if (!probe) {
      return false;
    }

    FileDto file;
    file.id = std::to_string(i);
    file.file_name = file_paths[i].filename().string();
    file.size = static_cast<std::uint64_t>(std::filesystem::file_size(file_paths[i]));
    file.file_type = mime_from_filename(file.file_name);
    files.push_back(std::move(file));
  }

  PrepareUploadRequestDto request;
  request.info = self;
  for (const auto& file : files) {
    request.files.emplace(file.id, file);
  }

  const bool v2 = target_uses_v2_api(target);
  const HttpResult prepared = http_post(target.ip, target.port, v2 ? kRoutePrepareUpload : kRoutePrepareUploadV1, to_json(request).dump());
  if (prepared.status != 200) {
    return false;
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
    return false;
  }

  bool all_uploaded = true;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const FileDto& file = files[i];
    const auto token = response.files.find(file.id);
    if (token == response.files.end()) {
      all_uploaded = false;
      continue;
    }

    std::ifstream in(file_paths[i], std::ios::binary);
    if (!in) {
      return false;
    }

    std::map<std::string, std::string> query = {
        {"fileId", file.id},
        {"token", token->second},
    };
    if (v2) {
      query.emplace("sessionId", response.session_id);
    }
    const std::string path = with_query(v2 ? kRouteUpload : kRouteUploadV1, query);
    const HttpResult uploaded = post_file_raw(target.ip, target.port, path, in, file.size, file.file_type);
    if (uploaded.status != 200) {
      all_uploaded = false;
    }
  }

  return all_uploaded;
}

bool send_single_file_http(const Device& target, const std::filesystem::path& file_path, const InfoRegisterDto& self) {
  return send_files_http(target, {file_path}, self);
}

} // namespace localsend
