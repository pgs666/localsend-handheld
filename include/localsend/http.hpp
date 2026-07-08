#pragma once

#include "localsend/protocol.hpp"
#include "localsend/tls.hpp"
#include "localsend/transfer.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#if LOCALSEND_PLATFORM_PSV
#include <pthread.h>
#else
#include <thread>
#endif
#include <vector>

namespace localsend {

class HttpStream;

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> headers;
  std::string body;
  std::size_t content_length = 0;
  bool chunked = false;
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

struct SendFilesResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
};

struct SendFilesControl {
  std::atomic<bool> cancel_requested{false};
};

HttpResult http_get(const std::string& host, int port, const std::string& path);
HttpResult https_get(const std::string& host, int port, const std::string& path, const std::string& expected_fingerprint = "");
HttpResult http_post(const std::string& host,
                     int port,
                     const std::string& path,
                     const std::string& body,
                     const std::string& content_type = "application/json");
HttpResult https_post(const std::string& host,
                      int port,
                      const std::string& path,
                      const std::string& body,
                      const std::string& content_type = "application/json",
                      const std::string& expected_fingerprint = "");
HttpResult http_post_chunked(const std::string& host,
                             int port,
                             const std::string& path,
                             const std::vector<std::string>& chunks,
                             const std::string& content_type = "application/octet-stream");

class LocalSendServer {
public:
  LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox);
  LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox, TransferStore* transfers);
  LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox, TlsCredentials tls_credentials);
  LocalSendServer(InfoRegisterDto self, std::filesystem::path inbox, TlsCredentials tls_credentials, TransferStore* transfers);
  ~LocalSendServer();

  LocalSendServer(const LocalSendServer&) = delete;
  LocalSendServer& operator=(const LocalSendServer&) = delete;

  bool start(int requested_port);
  void stop();
  void poll_once();
  int port() const { return port_; }
  void set_register_callback(std::function<void(Device)> callback);

private:
  struct PendingFile {
    FileDto dto;
    std::string token;
    std::filesystem::path destination;
    std::uint64_t transfer_id = 0;
    bool complete = false;
  };

  struct Session {
    std::string session_id;
    std::map<std::string, PendingFile> files;
    bool cancelled = false;
  };

  void accept_loop();
  void handle_client(int client_fd, std::string remote_ip);
  HttpResponse route(const HttpRequest& request, const std::string& remote_ip);
#if LOCALSEND_PLATFORM_PSV
  static void* accept_thread_entry(void* arg);
#endif

  HttpResponse handle_info(const HttpRequest& request) const;
  HttpResponse handle_register(const HttpRequest& request, const std::string& remote_ip);
  HttpResponse handle_prepare_upload(const HttpRequest& request, bool v2);
  HttpResponse handle_upload(HttpStream& stream, const HttpRequest& request, const std::string& initial_body, bool v2);
  HttpResponse handle_cancel(const HttpRequest& request);

  InfoRegisterDto self_;
  std::filesystem::path inbox_;
  std::optional<TlsCredentials> tls_credentials_;
  TransferStore* transfers_ = nullptr;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
#if LOCALSEND_PLATFORM_PSV
  pthread_t accept_thread_{};
  bool accept_thread_started_ = false;
#else
  std::thread accept_thread_;
#endif
  std::mutex sessions_mutex_;
  std::map<std::string, Session> sessions_;
  std::mutex register_callback_mutex_;
  std::function<void(Device)> register_callback_;
};

bool send_single_file_http(const Device& target, const std::filesystem::path& file_path, const InfoRegisterDto& self);
bool send_files_http(const Device& target, const std::vector<std::filesystem::path>& file_paths, const InfoRegisterDto& self);
bool send_files_http(const Device& target,
                     const std::vector<std::filesystem::path>& file_paths,
                     const InfoRegisterDto& self,
                     TransferStore* transfers);
SendFilesResult send_files_http_detailed(const Device& target,
                                         const std::vector<std::filesystem::path>& file_paths,
                                         const InfoRegisterDto& self,
                                         TransferStore* transfers,
                                         SendFilesControl* control = nullptr);

} // namespace localsend
