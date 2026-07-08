#include <switch.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

constexpr int kPort = 53317;
constexpr int kBufferSize = 64 * 1024;
constexpr int kMaxFiles = 16;
constexpr int kClientSocketTimeoutSeconds = 120;
constexpr const char* kInbox = "sdmc:/switch/localsend/inbox";
// Temporary protocol bring-up path. Remove after borealis device picker and file browser land.
constexpr const char* kOutbox = "sdmc:/switch/localsend/outbox";
constexpr const char* kOutboxTestFile = "sdmc:/switch/localsend/outbox/switch-test.txt";
constexpr const char* kTargetPath = "sdmc:/switch/localsend/target.txt";
constexpr const char* kDefaultSendTargetIp = "192.168.31.150";
constexpr const char* kLogPath = "sdmc:/switch/localsend/localsend.log";
constexpr const char* kMulticastGroup = "224.0.0.167";
constexpr const char* kBroadcastAddress = "255.255.255.255";

struct PendingFile {
  bool active = false;
  std::string map_key = "0";
  std::string file_id = "0";
  std::string token = "switch-token-0";
  std::string file_name = "localsend-upload.bin";
  size_t size = 0;
};

struct UploadSession {
  bool active = false;
  std::string session_id = "switch-session";
  PendingFile files[kMaxFiles];
  int file_count = 0;
};

UploadSession g_session;
int g_received_files = 0;
int g_sent_files = 0;
char g_status[256] = "Starting";
int g_announcements = 0;

struct ClientHttpResult {
  int status = 0;
  std::string body;
};

void append_log(const std::string& line) {
  FILE* out = std::fopen(kLogPath, "ab");
  if (!out) {
    return;
  }
  std::fwrite(line.data(), 1, line.size(), out);
  std::fwrite("\n", 1, 1, out);
  std::fclose(out);
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string json_escape(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    if (static_cast<unsigned char>(c) >= 0x20) {
      out.push_back(c);
    }
  }
  return out;
}

std::string sanitize_filename(const std::string& value) {
  std::string out;
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '/' || c == '\\') {
      continue;
    }
    out.push_back(c);
  }
  while (out.find("..") != std::string::npos) {
    out.erase(out.find(".."), 2);
  }
  while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
    out.pop_back();
  }
  return out.empty() ? "localsend-upload.bin" : out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::string url_decode(const std::string& value) {
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = hex_value(value[i + 1]);
      const int lo = hex_value(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i] == '+' ? ' ' : value[i]);
  }
  return out;
}

bool file_exists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

std::string unique_inbox_path(const std::string& file_name) {
  const std::string clean = sanitize_filename(file_name);
  const size_t dot = clean.find_last_of('.');
  const std::string stem = dot == std::string::npos || dot == 0 ? clean : clean.substr(0, dot);
  const std::string ext = dot == std::string::npos || dot == 0 ? "" : clean.substr(dot);
  std::string path = std::string(kInbox) + "/" + clean;
  if (!file_exists(path)) {
    return path;
  }

  for (int i = 1; i < 10000; ++i) {
    path = std::string(kInbox) + "/" + stem + " (" + std::to_string(i) + ")" + ext;
    if (!file_exists(path)) {
      return path;
    }
  }

  return std::string(kInbox) + "/" + stem + " (9999)" + ext;
}

std::string find_json_string(const std::string& body, const std::string& key, const std::string& fallback) {
  const std::string marker = "\"" + key + "\":";
  size_t pos = body.find(marker);
  if (pos == std::string::npos) {
    return fallback;
  }
  pos = body.find('"', pos + marker.size());
  if (pos == std::string::npos) {
    return fallback;
  }
  ++pos;
  std::string out;
  bool escape = false;
  for (; pos < body.size(); ++pos) {
    const char c = body[pos];
    if (escape) {
      out.push_back(c);
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      break;
    }
    out.push_back(c);
  }
  return out.empty() ? fallback : out;
}

std::string find_json_string_from(const std::string& body, size_t start, const std::string& key, const std::string& fallback) {
  const std::string marker = "\"" + key + "\":";
  size_t pos = body.find(marker, start);
  if (pos == std::string::npos) {
    return fallback;
  }
  pos = body.find('"', pos + marker.size());
  if (pos == std::string::npos) {
    return fallback;
  }
  ++pos;
  std::string out;
  bool escape = false;
  for (; pos < body.size(); ++pos) {
    const char c = body[pos];
    if (escape) {
      out.push_back(c);
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      break;
    }
    out.push_back(c);
  }
  return out.empty() ? fallback : out;
}

size_t find_json_size(const std::string& body, size_t fallback) {
  const std::string marker = "\"size\":";
  size_t pos = body.find(marker);
  if (pos == std::string::npos) {
    return fallback;
  }
  pos += marker.size();
  while (pos < body.size() && body[pos] == ' ') {
    ++pos;
  }
  return static_cast<size_t>(std::strtoull(body.c_str() + pos, nullptr, 10));
}

size_t find_json_size_from(const std::string& body, size_t start, size_t fallback) {
  const std::string marker = "\"size\":";
  size_t pos = body.find(marker, start);
  if (pos == std::string::npos) {
    return fallback;
  }
  pos += marker.size();
  while (pos < body.size() && body[pos] == ' ') {
    ++pos;
  }
  return static_cast<size_t>(std::strtoull(body.c_str() + pos, nullptr, 10));
}

size_t matching_brace(const std::string& body, size_t open_pos) {
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = open_pos; i < body.size(); ++i) {
    const char c = body[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_string && c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) {
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        return i;
      }
    }
  }
  return std::string::npos;
}

std::string previous_object_key(const std::string& body, size_t object_start, const std::string& fallback) {
  if (object_start == 0) {
    return fallback;
  }
  size_t colon = body.rfind(':', object_start);
  if (colon == std::string::npos) {
    return fallback;
  }
  size_t end = body.rfind('"', colon);
  if (end == std::string::npos || end == 0) {
    return fallback;
  }
  size_t begin = body.rfind('"', end - 1);
  if (begin == std::string::npos || begin >= end) {
    return fallback;
  }
  return body.substr(begin + 1, end - begin - 1);
}

size_t skip_ws(const std::string& body, size_t pos) {
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\n' || body[pos] == '\r' || body[pos] == '\t')) {
    ++pos;
  }
  return pos;
}

size_t parse_json_string_at(const std::string& body, size_t quote_pos, std::string& out) {
  if (quote_pos >= body.size() || body[quote_pos] != '"') {
    return std::string::npos;
  }
  out.clear();
  bool escape = false;
  for (size_t pos = quote_pos + 1; pos < body.size(); ++pos) {
    const char c = body[pos];
    if (escape) {
      out.push_back(c);
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      return pos + 1;
    }
    out.push_back(c);
  }
  return std::string::npos;
}

size_t files_object_start(const std::string& body) {
  size_t files = body.find("\"files\"");
  if (files == std::string::npos) {
    return std::string::npos;
  }
  size_t colon = body.find(':', files);
  if (colon == std::string::npos) {
    return std::string::npos;
  }
  size_t object_start = body.find('{', colon);
  if (object_start == std::string::npos) {
    return std::string::npos;
  }
  return object_start;
}

std::string query_value(const std::string& target, const std::string& key) {
  const size_t q = target.find('?');
  if (q == std::string::npos) {
    return "";
  }
  const std::string marker = key + "=";
  size_t pos = target.find(marker, q + 1);
  if (pos == std::string::npos) {
    return "";
  }
  pos += marker.size();
  const size_t end = target.find('&', pos);
  return url_decode(target.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

bool send_all(int fd, const char* data, size_t size) {
  while (size > 0) {
    const ssize_t sent = send(fd, data, size, 0);
    if (sent <= 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        svcSleepThread(1'000'000);
        continue;
      }
      return false;
    }
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

void set_blocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }
}

ssize_t recv_retry(int fd, char* buffer, size_t size) {
  while (true) {
    const ssize_t got = recv(fd, buffer, size, 0);
    if (got >= 0) {
      return got;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      svcSleepThread(1'000'000);
      continue;
    }
    return got;
  }
}

void send_response(int fd, int status, const char* content_type, const std::string& body) {
  const char* text = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden" : "Not Found";
  char header[256];
  std::snprintf(header,
                sizeof(header),
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                status,
                text,
                content_type,
                body.size());
  send_all(fd, header, std::strlen(header));
  send_all(fd, body.data(), body.size());
}

bool read_headers(int fd, std::string& request, std::string& initial_body) {
  char chunk[4096];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 1024 * 1024) {
    const ssize_t got = recv_retry(fd, chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    request.append(chunk, static_cast<size_t>(got));
  }
  const size_t split = request.find("\r\n\r\n");
  if (split == std::string::npos) {
    return false;
  }
  initial_body = request.substr(split + 4);
  request.resize(split + 4);
  return true;
}

int status_code_from_headers(const std::string& headers) {
  const size_t first_space = headers.find(' ');
  if (first_space == std::string::npos) {
    return 0;
  }
  return std::atoi(headers.c_str() + first_space + 1);
}

size_t content_length(const std::string& headers) {
  const std::string marker = "content-length:";
  std::string lower = headers;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  size_t pos = lower.find(marker);
  if (pos == std::string::npos) {
    return 0;
  }
  pos += marker.size();
  while (pos < headers.size() && headers[pos] == ' ') {
    ++pos;
  }
  return static_cast<size_t>(std::strtoull(headers.c_str() + pos, nullptr, 10));
}

bool transfer_encoding_chunked(const std::string& headers) {
  std::string lower = headers;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  const size_t pos = lower.find("transfer-encoding:");
  if (pos == std::string::npos) {
    return false;
  }
  const size_t line_end = lower.find("\r\n", pos);
  return lower.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos).find("chunked") != std::string::npos;
}

std::string read_body_string(int fd, std::string body, size_t length) {
  char chunk[4096];
  while (body.size() < length) {
    const ssize_t got = recv_retry(fd, chunk, sizeof(chunk));
    if (got <= 0) {
      break;
    }
    body.append(chunk, static_cast<size_t>(got));
  }
  if (body.size() > length) {
    body.resize(length);
  }
  return body;
}

bool read_chunked_body_string(int fd, const std::string& initial_body, std::string& body) {
  std::string buffer = initial_body;
  size_t pos = 0;
  body.clear();

  auto fill = [&](size_t need) {
    char chunk[4096];
    while (buffer.size() - pos < need) {
      const ssize_t got = recv_retry(fd, chunk, sizeof(chunk));
      if (got <= 0) {
        return false;
      }
      buffer.append(chunk, static_cast<size_t>(got));
    }
    return true;
  };

  while (true) {
    size_t line_end = buffer.find("\r\n", pos);
    while (line_end == std::string::npos) {
      if (!fill(buffer.size() - pos + 1)) {
        return false;
      }
      line_end = buffer.find("\r\n", pos);
    }

    std::string size_text = buffer.substr(pos, line_end - pos);
    const size_t semicolon = size_text.find(';');
    if (semicolon != std::string::npos) {
      size_text.resize(semicolon);
    }

    char* end = nullptr;
    const size_t chunk_size = static_cast<size_t>(std::strtoull(size_text.c_str(), &end, 16));
    if (end == size_text.c_str()) {
      return false;
    }

    pos = line_end + 2;
    if (chunk_size == 0) {
      return true;
    }

    if (!fill(chunk_size + 2)) {
      return false;
    }
    body.append(buffer.data() + pos, chunk_size);
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

ClientHttpResult read_client_response(int fd) {
  ClientHttpResult result;
  std::string headers;
  std::string initial_body;
  if (!read_headers(fd, headers, initial_body)) {
    return result;
  }
  result.status = status_code_from_headers(headers);
  if (transfer_encoding_chunked(headers)) {
    if (!read_chunked_body_string(fd, initial_body, result.body)) {
      result.body.clear();
    }
  } else {
    result.body = read_body_string(fd, initial_body, content_length(headers));
  }
  return result;
}

int connect_tcp(const std::string& ip, int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  timeval timeout{};
  timeout.tv_sec = kClientSocketTimeoutSeconds;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  return fd;
}

ClientHttpResult http_post_json(const std::string& ip, int port, const char* path, const std::string& body) {
  ClientHttpResult result;
  const int fd = connect_tcp(ip, port);
  if (fd < 0) {
    return result;
  }

  char header[512];
  std::snprintf(header,
                sizeof(header),
                "POST %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                path,
                ip.c_str(),
                port,
                body.size());
  if (send_all(fd, header, std::strlen(header)) && send_all(fd, body.data(), body.size())) {
    result = read_client_response(fd);
  }
  close(fd);
  return result;
}

ClientHttpResult http_get(const std::string& ip, int port, const char* path) {
  ClientHttpResult result;
  const int fd = connect_tcp(ip, port);
  if (fd < 0) {
    return result;
  }

  char header[512];
  std::snprintf(header,
                sizeof(header),
                "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                path,
                ip.c_str(),
                port);
  if (send_all(fd, header, std::strlen(header))) {
    result = read_client_response(fd);
  }
  close(fd);
  return result;
}

ClientHttpResult http_post_file(const std::string& ip, int port, const std::string& path, const std::string& file_path, size_t size) {
  ClientHttpResult result;
  FILE* in = std::fopen(file_path.c_str(), "rb");
  if (!in) {
    return result;
  }

  const int fd = connect_tcp(ip, port);
  if (fd < 0) {
    std::fclose(in);
    return result;
  }

  char header[512];
  std::snprintf(header,
                sizeof(header),
                "POST %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\n\r\n",
                path.c_str(),
                ip.c_str(),
                port,
                size);

  bool ok = send_all(fd, header, std::strlen(header));
  char* buffer = static_cast<char*>(std::malloc(kBufferSize));
  while (ok) {
    const size_t got = std::fread(buffer, 1, kBufferSize, in);
    if (got > 0) {
      ok = send_all(fd, buffer, got);
    }
    if (got < kBufferSize) {
      break;
    }
  }
  std::free(buffer);
  std::fclose(in);

  if (ok) {
    result = read_client_response(fd);
  }
  close(fd);
  return result;
}

bool fill_stream_buffer(int fd, std::string& buffer, size_t pos, size_t need) {
  char chunk[4096];
  while (buffer.size() - pos < need) {
    const ssize_t got = recv_retry(fd, chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(got));
  }
  return true;
}

bool read_chunked_body_to_file(int fd, const std::string& initial_body, FILE* out, size_t& written) {
  std::string buffer = initial_body;
  size_t pos = 0;
  written = 0;

  while (true) {
    size_t line_end = buffer.find("\r\n", pos);
    while (line_end == std::string::npos) {
      if (!fill_stream_buffer(fd, buffer, pos, buffer.size() - pos + 1)) {
        return false;
      }
      line_end = buffer.find("\r\n", pos);
    }

    std::string size_text = buffer.substr(pos, line_end - pos);
    const size_t semicolon = size_text.find(';');
    if (semicolon != std::string::npos) {
      size_text.resize(semicolon);
    }
    const size_t chunk_size = static_cast<size_t>(std::strtoull(size_text.c_str(), nullptr, 16));
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
    std::fwrite(buffer.data() + pos, 1, chunk_size, out);
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

std::string first_target(const std::string& request) {
  const size_t first_space = request.find(' ');
  if (first_space == std::string::npos) {
    return "/";
  }
  const size_t second_space = request.find(' ', first_space + 1);
  if (second_space == std::string::npos) {
    return "/";
  }
  return request.substr(first_space + 1, second_space - first_space - 1);
}

bool first_method_is(const std::string& request, const char* method) {
  return starts_with(request, method) && request[std::strlen(method)] == ' ';
}

std::string filename_from_path(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool read_send_target(std::string& ip, int& port) {
  ip = kDefaultSendTargetIp;
  port = kPort;
  FILE* in = std::fopen(kTargetPath, "rb");
  if (!in) {
    return true;
  }
  char ip_buf[128] = {};
  int parsed_port = 0;
  const int matched = std::fscanf(in, "%127s %d", ip_buf, &parsed_port);
  std::fclose(in);
  if (matched < 1) {
    return true;
  }
  ip = ip_buf;
  if (matched >= 2 && parsed_port > 0 && parsed_port <= 65535) {
    port = parsed_port;
  }
  return true;
}

bool first_outbox_file(std::string& path, size_t& size) {
  DIR* dir = opendir(kOutbox);
  if (!dir) {
    return false;
  }

  bool found = false;
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    const std::string candidate = std::string(kOutbox) + "/" + entry->d_name;
    struct stat st {};
    if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      path = candidate;
      size = static_cast<size_t>(st.st_size);
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

bool create_outbox_test_file(std::string& path, size_t& size) {
  mkdir("sdmc:/switch/localsend", 0777);
  mkdir(kOutbox, 0777);

  constexpr const char* kBody =
      "hello from LocalSend Handheld Switch test sender\n"
      "This file was created because the temporary outbox was empty.\n";
  FILE* out = std::fopen(kOutboxTestFile, "wb");
  if (!out) {
    return false;
  }
  const size_t expected = std::strlen(kBody);
  const size_t written = std::fwrite(kBody, 1, expected, out);
  const int closed = std::fclose(out);
  if (written != expected || closed != 0) {
    return false;
  }

  path = kOutboxTestFile;
  size = expected;
  append_log("send created outbox test file=" + path + " bytes=" + std::to_string(size));
  return true;
}

bool extract_prepare_response(const std::string& body, std::string& session_id, std::string& token) {
  session_id = find_json_string(body, "sessionId", "");
  const size_t files_start = files_object_start(body);
  if (files_start == std::string::npos) {
    return false;
  }
  size_t pos = files_start + 1;
  pos = skip_ws(body, pos);
  std::string key;
  const size_t after_key = parse_json_string_at(body, pos, key);
  if (after_key == std::string::npos) {
    return false;
  }
  const size_t colon = body.find(':', after_key);
  if (colon == std::string::npos) {
    return false;
  }
  pos = skip_ws(body, colon + 1);
  return parse_json_string_at(body, pos, token) != std::string::npos && !session_id.empty() && !token.empty();
}

bool extract_legacy_prepare_response(const std::string& body, std::string& token) {
  size_t pos = skip_ws(body, 0);
  if (pos >= body.size() || body[pos] != '{') {
    return false;
  }
  pos = skip_ws(body, pos + 1);
  std::string key;
  const size_t after_key = parse_json_string_at(body, pos, key);
  if (after_key == std::string::npos) {
    return false;
  }
  const size_t colon = body.find(':', after_key);
  if (colon == std::string::npos) {
    return false;
  }
  pos = skip_ws(body, colon + 1);
  return parse_json_string_at(body, pos, token) != std::string::npos && !token.empty();
}

void send_outbox_file() {
  // TODO: Replace this manual outbox sender with the real borealis send flow.
  std::string ip;
  int port = kPort;
  if (!read_send_target(ip, port)) {
    std::snprintf(g_status, sizeof(g_status), "Send failed: create target.txt");
    append_log("send failed: missing target file");
    return;
  }

  std::string file_path;
  size_t file_size = 0;
  if (!first_outbox_file(file_path, file_size)) {
    append_log("send outbox empty; creating test file");
    if (!create_outbox_test_file(file_path, file_size)) {
      std::snprintf(g_status, sizeof(g_status), "Send failed: outbox empty");
      append_log("send failed: outbox empty");
      return;
    }
  }

  const std::string file_name = sanitize_filename(filename_from_path(file_path));
  const std::string prepare =
      "{\"info\":{\"alias\":\"LocalSend Switch\",\"version\":\"2.1\",\"deviceModel\":\"Nintendo Switch\","
      "\"deviceType\":\"desktop\",\"fingerprint\":\"\",\"port\":53317,\"protocol\":\"http\",\"download\":false},"
      "\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"" + json_escape(file_name) +
      "\",\"size\":" + std::to_string(file_size) + ",\"fileType\":\"application/octet-stream\"}}}";

  bool use_v2 = true;
  ClientHttpResult info = http_get(ip, port, "/api/localsend/v2/info");
  if (info.status != 200) {
    append_log("send v2 info failed status=" + std::to_string(info.status) + " body=" + info.body.substr(0, 256));
    info = http_get(ip, port, "/api/localsend/v1/info");
    use_v2 = false;
  }
  if (info.status != 200) {
    std::snprintf(g_status, sizeof(g_status), "Send info failed: %d", info.status);
    append_log("send info failed status=" + std::to_string(info.status) + " body=" + info.body.substr(0, 256));
    return;
  }
  const std::string target_version = find_json_string(info.body, "version", use_v2 ? "2.1" : "1.0");
  append_log("send target info version=" + target_version + " v2=" + std::to_string(use_v2));

  std::snprintf(g_status, sizeof(g_status), "Preparing send: %s", file_name.c_str());
  append_log("send prepare target=" + ip + ":" + std::to_string(port) + " file=" + file_path + " size=" + std::to_string(file_size));
  const ClientHttpResult prepared = http_post_json(ip, port, use_v2 ? "/api/localsend/v2/prepare-upload" : "/api/localsend/v1/send-request", prepare);
  if (prepared.status != 200) {
    std::snprintf(g_status, sizeof(g_status), "Send prepare failed: %d", prepared.status);
    append_log("send prepare failed status=" + std::to_string(prepared.status) + " body=" + prepared.body.substr(0, 512));
    return;
  }

  std::string session_id;
  std::string token;
  const bool parsed = use_v2 ? extract_prepare_response(prepared.body, session_id, token) : extract_legacy_prepare_response(prepared.body, token);
  if (!parsed) {
    std::snprintf(g_status, sizeof(g_status), "Send failed: bad prepare response");
    append_log("send bad prepare response=" + prepared.body.substr(0, 512));
    return;
  }

  const std::string upload_path = use_v2
                                      ? "/api/localsend/v2/upload?sessionId=" + session_id + "&fileId=0&token=" + token
                                      : "/api/localsend/v1/send?fileId=0&token=" + token;
  std::snprintf(g_status, sizeof(g_status), "Sending: %s", file_name.c_str());
  const ClientHttpResult uploaded = http_post_file(ip, port, upload_path, file_path, file_size);
  if (uploaded.status != 200) {
    std::snprintf(g_status, sizeof(g_status), "Send upload failed: %d", uploaded.status);
    append_log("send upload failed status=" + std::to_string(uploaded.status) + " body=" + uploaded.body.substr(0, 512));
    return;
  }

  ++g_sent_files;
  std::snprintf(g_status, sizeof(g_status), "Sent: %s (%zu bytes)", file_name.c_str(), file_size);
  append_log("send ok file=" + file_path + " bytes=" + std::to_string(file_size));
}

void handle_info(int fd) {
  const std::string body =
      "{\"alias\":\"LocalSend Switch\",\"version\":\"2.1\",\"deviceModel\":\"Nintendo Switch\","
      "\"deviceType\":\"desktop\",\"fingerprint\":\"\",\"download\":false}";
  send_response(fd, 200, "application/json", body);
}

void handle_debug_log(int fd) {
  FILE* in = std::fopen(kLogPath, "rb");
  if (!in) {
    send_response(fd, 404, "text/plain", "log not found");
    return;
  }
  std::string body;
  char chunk[1024];
  while (true) {
    const size_t got = std::fread(chunk, 1, sizeof(chunk), in);
    if (got == 0) {
      break;
    }
    body.append(chunk, got);
    if (body.size() > 64 * 1024) {
      body.erase(0, body.size() - 64 * 1024);
    }
  }
  std::fclose(in);
  send_response(fd, 200, "text/plain", body);
}

void handle_prepare_upload(int fd, const std::string& body, bool v2) {
  append_log("prepare-upload body bytes=" + std::to_string(body.size()));
  append_log(body.substr(0, 4096));
  g_session.active = true;
  g_session.session_id = "switch-session";
  g_session.file_count = 0;

  const size_t files_start = files_object_start(body);
  const size_t files_end = files_start == std::string::npos ? std::string::npos : matching_brace(body, files_start);
  size_t pos = files_start == std::string::npos ? 0 : files_start + 1;
  while (g_session.file_count < kMaxFiles && pos < body.size() && (files_end == std::string::npos || pos < files_end)) {
    pos = skip_ws(body, pos);
    if (pos >= body.size() || body[pos] == '}') {
      break;
    }

    std::string map_key;
    const size_t after_key = parse_json_string_at(body, pos, map_key);
    if (after_key == std::string::npos) {
      break;
    }
    size_t colon = body.find(':', after_key);
    if (colon == std::string::npos || (files_end != std::string::npos && colon >= files_end)) {
      break;
    }
    const size_t object_start = body.find('{', colon);
    if (object_start == std::string::npos || (files_end != std::string::npos && object_start >= files_end)) {
      break;
    }
    const size_t object_end = matching_brace(body, object_start);
    if (object_end == std::string::npos) {
      break;
    }

    PendingFile& file = g_session.files[g_session.file_count];
    file.active = true;
    const std::string fallback_id = map_key.empty() ? previous_object_key(body, object_start, std::to_string(g_session.file_count)) : map_key;
    file.map_key = fallback_id;
    file.file_id = find_json_string_from(body, object_start, "id", fallback_id);
    if (object_end != std::string::npos && body.find("\"id\":", object_start) > object_end) {
      file.file_id = fallback_id;
    }
    file.token = "switch-token-" + std::to_string(g_session.file_count);
    file.file_name = sanitize_filename(find_json_string_from(body, object_start, "fileName", "localsend-upload.bin"));
    file.size = find_json_size_from(body, object_start, 0);
    ++g_session.file_count;
    pos = object_end + 1;
    const size_t comma = body.find(',', pos);
    if (comma == std::string::npos || (files_end != std::string::npos && comma >= files_end)) {
      break;
    }
    pos = comma + 1;
  }

  if (g_session.file_count == 0) {
    PendingFile& file = g_session.files[0];
    file.active = true;
    file.map_key = "0";
    file.file_id = find_json_string(body, "id", "0");
    file.token = "switch-token-0";
    file.file_name = sanitize_filename(find_json_string(body, "fileName", "localsend-upload.bin"));
    file.size = find_json_size(body, 0);
    g_session.file_count = 1;
  }

  std::string response = v2 ? "{\"sessionId\":\"" + g_session.session_id + "\",\"files\":{" : "{";
  for (int i = 0; i < g_session.file_count; ++i) {
    if (i > 0) {
      response += ",";
    }
    response += "\"" + json_escape(g_session.files[i].file_id) + "\":\"" + g_session.files[i].token + "\"";
    if (g_session.files[i].map_key != g_session.files[i].file_id) {
      response += ",\"" + json_escape(g_session.files[i].map_key) + "\":\"" + g_session.files[i].token + "\"";
    }
    const std::string ordinal = std::to_string(i);
    if (ordinal != g_session.files[i].file_id && ordinal != g_session.files[i].map_key) {
      response += ",\"" + ordinal + "\":\"" + g_session.files[i].token + "\"";
    }
    append_log("prepared index=" + std::to_string(i) + " mapKey=" + g_session.files[i].map_key +
               " fileId=" + g_session.files[i].file_id + " name=" + g_session.files[i].file_name +
               " size=" + std::to_string(g_session.files[i].size) + " token=" + g_session.files[i].token);
  }
  response += v2 ? "}}" : "}";
  append_log("prepare response=" + response);
  std::snprintf(g_status, sizeof(g_status), "Prepared: %d file(s)", g_session.file_count);
  send_response(fd, 200, "application/json", response);
}

void handle_upload(int fd, const std::string& target, const std::string& initial_body, size_t length, bool chunked, bool v2) {
  PendingFile* selected = nullptr;
  const std::string file_id = query_value(target, "fileId");
  const std::string token = query_value(target, "token");
  append_log("upload query fileId=" + file_id + " token=" + token + " length=" + std::to_string(length) + " chunked=" + std::to_string(chunked));
  for (int i = 0; i < g_session.file_count; ++i) {
    if (g_session.files[i].active && (g_session.files[i].file_id == file_id || g_session.files[i].map_key == file_id || std::to_string(i) == file_id) &&
        g_session.files[i].token == token) {
      selected = &g_session.files[i];
      break;
    }
  }

  if (!g_session.active || (v2 && query_value(target, "sessionId") != g_session.session_id) || selected == nullptr) {
    append_log("upload rejected active=" + std::to_string(g_session.active) + " sessionId=" + query_value(target, "sessionId"));
    send_response(fd, 403, "text/plain", "invalid session");
    return;
  }

  mkdir(kInbox, 0777);
  const std::string path = unique_inbox_path(selected->file_name);
  FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) {
    std::snprintf(g_status, sizeof(g_status), "Open failed: errno %d", errno);
    append_log("open failed path=" + path + " errno=" + std::to_string(errno));
    send_response(fd, 400, "text/plain", "open failed");
    return;
  }

  size_t written = 0;
  bool complete = true;
  if (chunked) {
    complete = read_chunked_body_to_file(fd, initial_body, out, written);
  } else {
    if (!initial_body.empty()) {
      const size_t count = initial_body.size() > length ? length : initial_body.size();
      std::fwrite(initial_body.data(), 1, count, out);
      written += count;
    }

    char* buffer = static_cast<char*>(std::malloc(kBufferSize));
    while (written < length) {
      const size_t remaining = length - written;
      const size_t want = remaining > kBufferSize ? kBufferSize : remaining;
      const ssize_t got = recv_retry(fd, buffer, want);
      if (got <= 0) {
        complete = false;
        break;
      }
      std::fwrite(buffer, 1, static_cast<size_t>(got), out);
      written += static_cast<size_t>(got);
    }
    std::free(buffer);
  }
  std::fclose(out);

  if (!complete || (!chunked && written != length)) {
    std::snprintf(g_status, sizeof(g_status), "Incomplete upload: %zu/%zu", written, length);
    append_log("upload incomplete path=" + path + " written=" + std::to_string(written) + " length=" + std::to_string(length));
    send_response(fd, 400, "text/plain", "incomplete upload");
    return;
  }

  ++g_received_files;
  selected->active = false;
  bool any_active = false;
  for (int i = 0; i < g_session.file_count; ++i) {
    any_active = any_active || g_session.files[i].active;
  }
  g_session.active = any_active;
  std::snprintf(g_status, sizeof(g_status), "Received: %s (%zu bytes)", selected->file_name.c_str(), written);
  append_log("upload ok path=" + path + " bytes=" + std::to_string(written));
  send_response(fd, 200, "application/json", "{}");
}

void handle_cancel(int fd) {
  g_session.active = false;
  std::snprintf(g_status, sizeof(g_status), "Session cancelled");
  send_response(fd, 200, "text/plain", "ok");
}

void handle_client(int fd) {
  std::string request;
  std::string initial_body;
  if (!read_headers(fd, request, initial_body)) {
    close(fd);
    return;
  }

  const std::string target = first_target(request);
  const size_t length = content_length(request);
  const bool chunked = transfer_encoding_chunked(request);
  append_log("request target=" + target + " length=" + std::to_string(length) + " chunked=" + std::to_string(chunked));

  if (first_method_is(request, "GET") && (starts_with(target, "/api/localsend/v2/info") || starts_with(target, "/api/localsend/v1/info"))) {
    handle_info(fd);
  } else if (first_method_is(request, "GET") && starts_with(target, "/debug/log")) {
    handle_debug_log(fd);
  } else if (first_method_is(request, "POST") && (starts_with(target, "/api/localsend/v2/register") || starts_with(target, "/api/localsend/v1/register"))) {
    handle_info(fd);
  } else if (first_method_is(request, "POST") && (starts_with(target, "/api/localsend/v2/prepare-upload") || starts_with(target, "/api/localsend/v1/send-request"))) {
    handle_prepare_upload(fd, read_body_string(fd, initial_body, length), starts_with(target, "/api/localsend/v2/prepare-upload"));
  } else if (first_method_is(request, "POST") && (starts_with(target, "/api/localsend/v2/upload") || starts_with(target, "/api/localsend/v1/send"))) {
    handle_upload(fd, target, initial_body, length, chunked, starts_with(target, "/api/localsend/v2/upload"));
  } else if (first_method_is(request, "POST") && (starts_with(target, "/api/localsend/v2/cancel") || starts_with(target, "/api/localsend/v1/cancel"))) {
    handle_cancel(fd);
  } else {
    send_response(fd, 404, "text/plain", "not found");
  }

  close(fd);
}

int create_server() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  int enabled = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kPort);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return fd;
}

bool send_discovery_packet(int fd, const char* address, const std::string& payload) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
    return false;
  }

  const ssize_t sent = sendto(fd,
                              payload.data(),
                              payload.size(),
                              0,
                              reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
  return sent == static_cast<ssize_t>(payload.size());
}

void send_announcement() {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return;
  }

  int enabled = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));

  unsigned char ttl = 1;
  unsigned char loop = 1;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  const std::string payload =
      "{\"alias\":\"LocalSend Switch\",\"version\":\"2.1\",\"deviceModel\":\"Nintendo Switch\","
      "\"deviceType\":\"desktop\",\"fingerprint\":\"\",\"port\":53317,\"protocol\":\"http\","
      "\"download\":false,\"announce\":true,\"announcement\":true}";
  bool sent_any = send_discovery_packet(fd, kMulticastGroup, payload);
  sent_any = send_discovery_packet(fd, kBroadcastAddress, payload) || sent_any;
  if (sent_any) {
    ++g_announcements;
  }
  close(fd);
}

void draw_status() {
  consoleClear();
  std::printf("LocalSend Handheld\n");
  std::printf("Switch HTTP receive MVP\n\n");
  std::printf("Listening: http://<switch-ip>:%d\n", kPort);
  std::printf("Discovery: multicast+broadcast announcements=%d\n", g_announcements);
  std::printf("Inbox: %s\n", kInbox);
  std::printf("Outbox: %s\n", kOutbox);
  std::printf("Send target: %s\n", kTargetPath);
  std::printf("Encryption must be disabled on peers.\n\n");
  std::printf("Files received: %d\n", g_received_files);
  std::printf("Files sent: %d\n", g_sent_files);
  std::printf("Status: %s\n\n", g_status);
  std::printf("Press X to send first outbox file.\n");
  std::printf("Press + to exit.\n");
  consoleUpdate(nullptr);
}

} // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  consoleInit(nullptr);
  socketInitializeDefault();
  mkdir("sdmc:/switch/localsend", 0777);
  mkdir(kInbox, 0777);
  mkdir(kOutbox, 0777);
  append_log("app started");

  int server = create_server();
  if (server < 0) {
    std::snprintf(g_status, sizeof(g_status), "Listen failed: errno %d", errno);
  } else {
    std::snprintf(g_status, sizeof(g_status), "Ready");
  }
  send_announcement();

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  draw_status();

  int frames = 0;
  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 pressed = padGetButtonsDown(&pad);
    if (pressed & HidNpadButton_Plus) {
      break;
    }
    if (pressed & HidNpadButton_X) {
      send_outbox_file();
      draw_status();
    }

    if (server >= 0) {
      const int client = accept(server, nullptr, nullptr);
      if (client >= 0) {
        set_blocking(client);
        std::snprintf(g_status, sizeof(g_status), "Handling request");
        draw_status();
        handle_client(client);
      }
    }

    if ((frames % 120) == 0) {
      send_announcement();
    }
    if ((frames++ % 30) == 0) {
      draw_status();
    }
    svcSleepThread(16'000'000);
  }

  if (server >= 0) {
    close(server);
  }
  socketExit();
  consoleExit(nullptr);
  return 0;
}
