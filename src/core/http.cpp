#include "localsend/http.hpp"

#include "localsend/constants.hpp"
#include "localsend/safe_path.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace localsend {
namespace {

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
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

std::size_t content_length(const std::map<std::string, std::string>& headers) {
  const auto it = headers.find("Content-Length");
  if (it == headers.end()) {
    return 0;
  }
  return static_cast<std::size_t>(std::stoull(it->second));
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

bool parse_request(int fd, HttpRequest& request) {
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

  return read_body(fd, buffer, header_end, content_length(request.headers), request.body);
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
  HttpResponse response = parse_request(client_fd, request) ? route(request) : text_response(400, "bad request");

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
  if (request.method == "GET" && request.path == kRouteInfo) {
    return handle_info();
  }
  if (request.method == "POST" && request.path == kRouteRegister) {
    return handle_info();
  }
  if (request.method == "POST" && request.path == kRoutePrepareUpload) {
    return handle_prepare_upload(request);
  }
  if (request.method == "POST" && request.path == kRouteUpload) {
    return handle_upload(request);
  }
  if (request.method == "POST" && request.path == kRouteCancel) {
    return handle_cancel(request);
  }
  return text_response(404, "not found");
}

HttpResponse LocalSendServer::handle_info() const {
  return json_response(200, to_json(static_cast<const InfoDto&>(self_)));
}

HttpResponse LocalSendServer::handle_prepare_upload(const HttpRequest& request) {
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
    session.files.emplace(entry.first, pending);
    response.files.emplace(entry.first, pending.token);
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.emplace(session.session_id, std::move(session));
  }

  return json_response(200, to_json(response));
}

HttpResponse LocalSendServer::handle_upload(const HttpRequest& request) {
  const auto session_it = request.query.find("sessionId");
  const auto file_it = request.query.find("fileId");
  const auto token_it = request.query.find("token");
  if (session_it == request.query.end() || file_it == request.query.end() || token_it == request.query.end()) {
    return text_response(400, "missing upload query");
  }

  std::filesystem::path destination;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session = sessions_.find(session_it->second);
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
    destination = file->second.destination;
    file->second.complete = true;
  }

  std::ofstream out(destination, std::ios::binary);
  if (!out) {
    return text_response(500, "failed to open destination");
  }
  out.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
  if (!out) {
    return text_response(500, "failed to write destination");
  }

  return text_response(200, "ok");
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

bool send_single_file_http(const Device& target, const std::filesystem::path& file_path, const InfoRegisterDto& self) {
  if (target.https) {
    return false;
  }

  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    return false;
  }
  const auto size = std::filesystem::file_size(file_path);

  FileDto file;
  file.id = "0";
  file.file_name = file_path.filename().string();
  file.size = static_cast<std::uint64_t>(size);
  file.file_type = mime_from_filename(file.file_name);

  PrepareUploadRequestDto request;
  request.info = self;
  request.files.emplace(file.id, file);

  const HttpResult prepared = http_post(target.ip, target.port, kRoutePrepareUpload, to_json(request).dump());
  if (prepared.status != 200) {
    return false;
  }

  PrepareUploadResponseDto response;
  try {
    response = prepare_upload_response_from_json(Json::parse(prepared.body));
  } catch (const std::exception&) {
    return false;
  }

  const auto token = response.files.find(file.id);
  if (token == response.files.end()) {
    return false;
  }

  std::ostringstream body;
  body << in.rdbuf();

  const std::string path = with_query(kRouteUpload, {
      {"sessionId", response.session_id},
      {"fileId", file.id},
      {"token", token->second},
  });
  const HttpResult uploaded = http_post(target.ip, target.port, path, body.str(), file.file_type);
  return uploaded.status == 200;
}

} // namespace localsend

