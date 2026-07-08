#pragma once

#include "localsend/protocol.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace localsend {

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> headers;
  std::string body;
  std::size_t content_length = 0;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

struct HttpResult {
  int status = 0;
  std::string body;
};

HttpResult http_get(const std::string& host, int port, const std::string& path);
HttpResult http_post(const std::string& host,
                     int port,
                     const std::string& path,
                     const std::string& body,
                     const std::string& content_type = "application/json");

class LocalSendServer {
public:
  LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox);
  ~LocalSendServer();

  LocalSendServer(const LocalSendServer&) = delete;
  LocalSendServer& operator=(const LocalSendServer&) = delete;

  bool start(int requested_port);
  void stop();
  int port() const { return port_; }

private:
  struct PendingFile {
    FileDto dto;
    std::string token;
    std::filesystem::path destination;
    bool complete = false;
  };

  struct Session {
    std::string session_id;
    std::map<std::string, PendingFile> files;
    bool cancelled = false;
  };

  void accept_loop();
  void handle_client(int client_fd);
  HttpResponse route(const HttpRequest& request);

  HttpResponse handle_info() const;
  HttpResponse handle_prepare_upload(const HttpRequest& request);
  HttpResponse handle_upload(int client_fd, const HttpRequest& request, const std::string& initial_body);
  HttpResponse handle_cancel(const HttpRequest& request);

  InfoRegisterDto self_;
  std::filesystem::path inbox_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::mutex sessions_mutex_;
  std::map<std::string, Session> sessions_;
};

bool send_single_file_http(const Device& target, const std::filesystem::path& file_path, const InfoRegisterDto& self);

} // namespace localsend
