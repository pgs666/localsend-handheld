#include <switch.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int kPort = 53317;
constexpr int kBufferSize = 64 * 1024;
constexpr int kMaxFiles = 16;
constexpr const char* kInbox = "sdmc:/switch/localsend/inbox";
constexpr const char* kMulticastGroup = "224.0.0.167";

struct PendingFile {
  bool active = false;
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
char g_status[256] = "Starting";
int g_announcements = 0;

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
      return false;
    }
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
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
    const ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
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

size_t content_length(const std::string& headers) {
  const std::string marker = "Content-Length:";
  size_t pos = headers.find(marker);
  if (pos == std::string::npos) {
    return 0;
  }
  pos += marker.size();
  while (pos < headers.size() && headers[pos] == ' ') {
    ++pos;
  }
  return static_cast<size_t>(std::strtoull(headers.c_str() + pos, nullptr, 10));
}

std::string read_body_string(int fd, std::string body, size_t length) {
  char chunk[4096];
  while (body.size() < length) {
    const ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
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

void handle_info(int fd) {
  const std::string body =
      "{\"alias\":\"LocalSend Switch\",\"version\":\"2.1\",\"deviceModel\":\"Nintendo Switch\","
      "\"deviceType\":\"desktop\",\"fingerprint\":\"\",\"download\":false}";
  send_response(fd, 200, "application/json", body);
}

void handle_prepare_upload(int fd, const std::string& body) {
  g_session.active = true;
  g_session.session_id = "switch-session";
  g_session.file_count = 0;

  size_t pos = 0;
  while (g_session.file_count < kMaxFiles) {
    pos = body.find("\"fileName\":", pos);
    if (pos == std::string::npos) {
      break;
    }

    const size_t object_start = body.rfind('{', pos);
    PendingFile& file = g_session.files[g_session.file_count];
    file.active = true;
    file.file_id = find_json_string_from(body, object_start == std::string::npos ? pos : object_start, "id", std::to_string(g_session.file_count));
    file.token = "switch-token-" + std::to_string(g_session.file_count);
    file.file_name = sanitize_filename(find_json_string_from(body, pos, "fileName", "localsend-upload.bin"));
    file.size = find_json_size_from(body, pos, 0);
    ++g_session.file_count;
    pos += 11;
  }

  if (g_session.file_count == 0) {
    PendingFile& file = g_session.files[0];
    file.active = true;
    file.file_id = find_json_string(body, "id", "0");
    file.token = "switch-token-0";
    file.file_name = sanitize_filename(find_json_string(body, "fileName", "localsend-upload.bin"));
    file.size = find_json_size(body, 0);
    g_session.file_count = 1;
  }

  std::string response = "{\"sessionId\":\"" + g_session.session_id + "\",\"files\":{";
  for (int i = 0; i < g_session.file_count; ++i) {
    if (i > 0) {
      response += ",";
    }
    response += "\"" + json_escape(g_session.files[i].file_id) + "\":\"" + g_session.files[i].token + "\"";
  }
  response += "}}";
  std::snprintf(g_status, sizeof(g_status), "Prepared: %d file(s)", g_session.file_count);
  send_response(fd, 200, "application/json", response);
}

void handle_upload(int fd, const std::string& target, const std::string& initial_body, size_t length) {
  PendingFile* selected = nullptr;
  const std::string file_id = query_value(target, "fileId");
  const std::string token = query_value(target, "token");
  for (int i = 0; i < g_session.file_count; ++i) {
    if (g_session.files[i].active && g_session.files[i].file_id == file_id && g_session.files[i].token == token) {
      selected = &g_session.files[i];
      break;
    }
  }

  if (!g_session.active || query_value(target, "sessionId") != g_session.session_id || selected == nullptr) {
    send_response(fd, 403, "text/plain", "invalid session");
    return;
  }

  mkdir(kInbox, 0777);
  const std::string path = unique_inbox_path(selected->file_name);
  FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) {
    std::snprintf(g_status, sizeof(g_status), "Open failed: errno %d", errno);
    send_response(fd, 400, "text/plain", "open failed");
    return;
  }

  size_t written = 0;
  if (!initial_body.empty()) {
    const size_t count = initial_body.size() > length ? length : initial_body.size();
    std::fwrite(initial_body.data(), 1, count, out);
    written += count;
  }

  char* buffer = static_cast<char*>(std::malloc(kBufferSize));
  while (written < length) {
    const size_t remaining = length - written;
    const size_t want = remaining > kBufferSize ? kBufferSize : remaining;
    const ssize_t got = recv(fd, buffer, want, 0);
    if (got <= 0) {
      break;
    }
    std::fwrite(buffer, 1, static_cast<size_t>(got), out);
    written += static_cast<size_t>(got);
  }
  std::free(buffer);
  std::fclose(out);

  if (written != length) {
    std::snprintf(g_status, sizeof(g_status), "Incomplete upload: %zu/%zu", written, length);
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
  send_response(fd, 200, "text/plain", "ok");
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

  if (first_method_is(request, "GET") && starts_with(target, "/api/localsend/v2/info")) {
    handle_info(fd);
  } else if (first_method_is(request, "POST") && starts_with(target, "/api/localsend/v2/register")) {
    handle_info(fd);
  } else if (first_method_is(request, "POST") && starts_with(target, "/api/localsend/v2/prepare-upload")) {
    handle_prepare_upload(fd, read_body_string(fd, initial_body, length));
  } else if (first_method_is(request, "POST") && starts_with(target, "/api/localsend/v2/upload")) {
    handle_upload(fd, target, initial_body, length);
  } else if (first_method_is(request, "POST") && starts_with(target, "/api/localsend/v2/cancel")) {
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

void send_announcement() {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  if (inet_pton(AF_INET, kMulticastGroup, &addr.sin_addr) != 1) {
    close(fd);
    return;
  }

  const std::string payload =
      "{\"alias\":\"LocalSend Switch\",\"version\":\"2.1\",\"deviceModel\":\"Nintendo Switch\","
      "\"deviceType\":\"desktop\",\"fingerprint\":\"\",\"port\":53317,\"protocol\":\"http\","
      "\"download\":false,\"announce\":true}";
  const ssize_t sent = sendto(fd,
                              payload.data(),
                              payload.size(),
                              0,
                              reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
  if (sent == static_cast<ssize_t>(payload.size())) {
    ++g_announcements;
  }
  close(fd);
}

void draw_status() {
  consoleClear();
  std::printf("LocalSend Handheld\n");
  std::printf("Switch HTTP receive MVP\n\n");
  std::printf("Listening: http://<switch-ip>:%d\n", kPort);
  std::printf("Discovery: %s:%d announcements=%d\n", kMulticastGroup, kPort, g_announcements);
  std::printf("Inbox: %s\n", kInbox);
  std::printf("Encryption must be disabled on peers.\n\n");
  std::printf("Files received: %d\n", g_received_files);
  std::printf("Status: %s\n\n", g_status);
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

    if (server >= 0) {
      const int client = accept(server, nullptr, nullptr);
      if (client >= 0) {
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
