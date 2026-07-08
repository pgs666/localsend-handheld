#include "localsend/constants.hpp"
#include "localsend/config.hpp"
#include "localsend/discovery.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"
#include "localsend/safe_path.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_json_round_trip() {
  const auto json = localsend::Json::parse(R"({"alias":"Switch","port":53317,"download":false,"items":["a",2,null]})");
  require(json.at("alias").as_string() == "Switch", "string field parse failed");
  require(json.at("port").as_int64() == 53317, "number field parse failed");
  require(!json.at("download").as_bool(), "bool field parse failed");
  require(json.at("items").as_array().size() == 3, "array parse failed");
  require(localsend::Json::parse(json.dump()).at("alias").as_string() == "Switch", "dump parse failed");
}

void test_info_dto() {
  localsend::InfoDto info;
  info.alias = "PS Vita";
  info.device_model = "Vita";
  info.device_type = localsend::DeviceType::Mobile;

  const auto json = localsend::to_json(info);
  require(json.at("version").as_string() == localsend::kProtocolVersion, "info version mismatch");
  require(json.at("fingerprint").as_string().empty(), "fingerprint should be empty in HTTP MVP");

  const auto decoded = localsend::info_from_json(json);
  require(decoded.alias == "PS Vita", "info alias decode failed");
  require(decoded.device_type == localsend::DeviceType::Mobile, "device type decode failed");
}

void test_prepare_upload_dto() {
  localsend::PrepareUploadRequestDto request;
  request.info.alias = "Desktop";
  request.info.port = 53317;
  request.info.protocol = localsend::ProtocolType::Http;

  localsend::FileDto file;
  file.id = "0";
  file.file_name = "photo.jpg";
  file.size = 42;
  file.file_type = localsend::mime_from_filename(file.file_name);
  request.files.emplace(file.id, file);

  const auto decoded = localsend::prepare_upload_request_from_json(localsend::Json::parse(localsend::to_json(request).dump()));
  require(decoded.info.alias == "Desktop", "prepare-upload info decode failed");
  require(decoded.files.at("0").file_type == "image/jpeg", "prepare-upload file MIME decode failed");
}

void test_prepare_upload_response_dto() {
  localsend::PrepareUploadResponseDto response;
  response.session_id = "session";
  response.files.emplace("0", "token");

  const auto decoded = localsend::prepare_upload_response_from_json(localsend::Json::parse(localsend::to_json(response).dump()));
  require(decoded.session_id == "session", "prepare-upload response session decode failed");
  require(decoded.files.at("0") == "token", "prepare-upload response token decode failed");
}

void test_multicast_dto() {
  localsend::MulticastDto dto;
  dto.alias = "Switch";
  dto.device_model = "Nintendo Switch";
  dto.port = 53317;
  dto.protocol = localsend::ProtocolType::Http;
  dto.announce = true;

  const auto payload = localsend::make_multicast_announcement(dto);
  const auto device = localsend::device_from_multicast(payload, "192.168.1.50", 53317);
  require(device.ip == "192.168.1.50", "multicast ip failed");
  require(device.alias == "Switch", "multicast alias failed");
  require(device.port == 53317, "multicast port failed");
  require(!device.https, "multicast protocol failed");
}

void test_route_constants() {
  require(std::string(localsend::kRouteInfo) == "/api/localsend/v2/info", "info route mismatch");
  require(std::string(localsend::kRoutePrepareUpload) == "/api/localsend/v2/prepare-upload", "prepare route mismatch");
  require(std::string(localsend::kRouteInfoV1) == "/api/localsend/v1/info", "v1 info route mismatch");
  require(std::string(localsend::kRoutePrepareUploadV1) == "/api/localsend/v1/send-request", "v1 prepare route mismatch");
  require(std::string(localsend::kDefaultMulticastGroup) == "224.0.0.167", "multicast group mismatch");
}

void test_default_config_paths() {
  const auto switch_config = localsend::default_config(localsend::PlatformKind::Switch);
  require(switch_config.inbox_path.string() == "sdmc:/switch/localsend/inbox/", "switch inbox path mismatch");
  require(switch_config.config_path.string() == "sdmc:/switch/localsend/config.json", "switch config path mismatch");

  const auto psv_config = localsend::default_config(localsend::PlatformKind::Psv);
  require(psv_config.inbox_path.string() == "ux0:data/localsend/inbox/", "psv inbox path mismatch");
  require(psv_config.config_path.string() == "ux0:data/localsend/config.json", "psv config path mismatch");
}

void test_config_round_trip() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-config-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "config.json";

  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Desk";
  config.inbox_path = "downloads";
  config.port = 12345;
  config.discovery_enabled = false;
  config.auto_accept = true;
  localsend::save_config(config, path);

  const auto loaded = localsend::load_config(localsend::PlatformKind::Desktop, path);
  require(loaded.alias == "Desk", "config alias round trip failed");
  require(loaded.inbox_path.string() == "downloads", "config inbox round trip failed");
  require(loaded.port == 12345, "config port round trip failed");
  require(!loaded.discovery_enabled, "config discovery round trip failed");
  require(loaded.auto_accept, "config auto accept round trip failed");

  std::filesystem::remove_all(dir);
}

void test_safe_filename() {
  require(localsend::sanitize_filename("../bad/name.txt") == "badname.txt", "path traversal sanitize failed");
  require(localsend::sanitize_filename("..") == "file", "empty sanitize fallback failed");
  require(localsend::sanitize_filename("中文.txt") == "中文.txt", "UTF-8 filename should be preserved");
}

void test_unique_destination() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-tests";
  std::filesystem::create_directories(dir);
  const auto existing = dir / "file.txt";
  {
    std::ofstream out(existing);
    out << "x";
  }
  const auto unique = localsend::unique_destination_path(dir, "file.txt");
  require(unique.filename().string() == "file (1).txt", "unique destination suffix failed");
  std::filesystem::remove(existing);
  std::filesystem::remove_all(dir);
}

void test_http_server_routes_and_upload() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.port = 0;
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start");

  const auto info = localsend::http_get("127.0.0.1", server.port(), localsend::kRouteInfo);
  require(info.status == 200, "info route failed");
  require(localsend::info_from_json(localsend::Json::parse(info.body)).alias == "Receiver", "info body failed");

  const auto source = dir / "source.txt";
  std::vector<char> expected(localsend::kTransferBufferSize + 1234);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<char>(i % 251);
  }
  {
    std::ofstream out(source, std::ios::binary);
    out.write(expected.data(), static_cast<std::streamsize>(expected.size()));
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = server.port();
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  require(localsend::send_single_file_http(target, source, sender), "single file HTTP send failed");
  const auto received = dir / "source (1).txt";
  require(std::filesystem::exists(received), "uploaded file missing");
  std::ifstream in(received, std::ios::binary);
  std::vector<char> actual((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  require(actual == expected, "uploaded file content mismatch");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_send_multiple_files() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-multi-tests";
  const auto source_dir = dir / "source";
  const auto inbox_dir = dir / "inbox";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(source_dir);
  std::filesystem::create_directories(inbox_dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, inbox_dir);
  require(server.start(0), "server failed to start for multi send test");

  const auto first = source_dir / "first.txt";
  const auto second = source_dir / "second.bin";
  {
    std::ofstream out(first, std::ios::binary);
    out << "first file";
  }
  {
    std::ofstream out(second, std::ios::binary);
    for (int i = 0; i < 70000; ++i) {
      out.put(static_cast<char>(i % 251));
    }
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = server.port();
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  require(localsend::send_files_http(target, {first, second}, sender), "multi file HTTP send failed");
  require(std::filesystem::exists(inbox_dir / "first.txt"), "first uploaded file missing");
  require(std::filesystem::exists(inbox_dir / "second.bin"), "second uploaded file missing");
  require(std::filesystem::file_size(inbox_dir / "first.txt") == std::filesystem::file_size(first), "first uploaded size mismatch");
  require(std::filesystem::file_size(inbox_dir / "second.bin") == std::filesystem::file_size(second), "second uploaded size mismatch");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_v1_legacy_routes() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-v1-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.port = 0;
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start for v1 test");

  const auto info = localsend::http_get("127.0.0.1", server.port(), std::string(localsend::kRouteInfoV1) + "?fingerprint=iphone");
  require(info.status == 200, "v1 info route failed");
  require(localsend::info_from_json(localsend::Json::parse(info.body)).version == localsend::kProtocolVersion, "v1 info should advertise current version");

  localsend::PrepareUploadRequestDto request;
  request.info.alias = "Legacy Sender";
  request.info.port = 12345;
  request.info.protocol = localsend::ProtocolType::Http;

  localsend::FileDto file;
  file.id = "legacy-file";
  file.file_name = "legacy.txt";
  file.size = 11;
  file.file_type = "text/plain";
  request.files.emplace(file.id, file);

  const auto prepare = localsend::http_post("127.0.0.1", server.port(), localsend::kRoutePrepareUploadV1, localsend::to_json(request).dump());
  require(prepare.status == 200, "v1 send-request failed");
  const auto token_map = localsend::Json::parse(prepare.body);
  require(token_map.contains(file.id), "v1 send-request must return raw token map");
  require(!token_map.contains("sessionId"), "v1 send-request must not return v2 wrapper");

  const std::string body = "hello world";
  const std::string upload_path = std::string(localsend::kRouteUploadV1) + "?fileId=" + file.id + "&token=" + token_map.at(file.id).as_string();
  const auto upload = localsend::http_post("127.0.0.1", server.port(), upload_path, body, file.file_type);
  require(upload.status == 200, "v1 send upload failed");
  require(std::filesystem::exists(dir / "legacy.txt"), "v1 uploaded file missing");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_cancel_rejects_upload() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-cancel-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start for cancel test");

  localsend::PrepareUploadRequestDto request;
  request.info.alias = "Sender";
  request.info.port = 12345;
  request.info.protocol = localsend::ProtocolType::Http;

  localsend::FileDto file;
  file.id = "cancel-file";
  file.file_name = "cancel.txt";
  file.size = 5;
  file.file_type = "text/plain";
  request.files.emplace(file.id, file);

  const auto prepare = localsend::http_post("127.0.0.1", server.port(), localsend::kRoutePrepareUpload, localsend::to_json(request).dump());
  require(prepare.status == 200, "prepare upload cancel test failed");
  const auto response = localsend::prepare_upload_response_from_json(localsend::Json::parse(prepare.body));
  const std::string token = response.files.at(file.id);

  const std::string cancel_path = std::string(localsend::kRouteCancel) + "?sessionId=" + response.session_id;
  const auto cancel = localsend::http_post("127.0.0.1", server.port(), cancel_path, "");
  require(cancel.status == 200, "cancel request failed");

  const std::string upload_path = std::string(localsend::kRouteUpload) + "?sessionId=" + response.session_id + "&fileId=" + file.id + "&token=" + token;
  const auto upload = localsend::http_post("127.0.0.1", server.port(), upload_path, "hello", file.file_type);
  require(upload.status != 200, "cancelled session upload should fail");
  require(!std::filesystem::exists(dir / "cancel.txt"), "cancelled upload should not create file");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_send_to_v1_target() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-send-v1-tests";
  const auto source_dir = dir / "source";
  const auto inbox_dir = dir / "inbox";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(source_dir);
  std::filesystem::create_directories(inbox_dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, inbox_dir);
  require(server.start(0), "server failed to start for v1 send target test");

  const auto source = source_dir / "legacy-target.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "legacy target";
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.version = "1.0";
  target.port = server.port();
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  require(localsend::send_single_file_http(target, source, sender), "send to v1 target failed");
  require(std::filesystem::exists(inbox_dir / "legacy-target.txt"), "v1 target uploaded file missing");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_prepare_uses_file_id() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-id-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start for id test");

  localsend::PrepareUploadRequestDto request;
  request.info.alias = "Sender";
  request.info.port = 12345;
  request.info.protocol = localsend::ProtocolType::Http;

  localsend::FileDto file;
  file.id = "official-file-id";
  file.file_name = "official.txt";
  file.size = 4;
  file.file_type = "text/plain";
  request.files.emplace("map-key-that-must-not-be-used", file);

  const auto prepare = localsend::http_post("127.0.0.1", server.port(), localsend::kRoutePrepareUpload, localsend::to_json(request).dump());
  require(prepare.status == 200, "prepare upload id test failed");
  const auto response = localsend::prepare_upload_response_from_json(localsend::Json::parse(prepare.body));
  require(response.files.count("official-file-id") == 1, "prepare response must use FileDto.id");
  require(response.files.count("map-key-that-must-not-be-used") == 0, "prepare response must not use request map key");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_server_chunked_upload() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-chunked-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start for chunked test");

  const std::vector<std::string> chunks = {"hello ", "chunked ", "localsend"};
  std::string expected;
  for (const auto& chunk : chunks) {
    expected += chunk;
  }

  localsend::PrepareUploadRequestDto request;
  request.info.alias = "Sender";
  request.info.port = 12345;
  request.info.protocol = localsend::ProtocolType::Http;

  localsend::FileDto file;
  file.id = "chunked-file-id";
  file.file_name = "chunked.txt";
  file.size = expected.size();
  file.file_type = "text/plain";
  request.files.emplace(file.id, file);

  const auto prepare = localsend::http_post("127.0.0.1", server.port(), localsend::kRoutePrepareUpload, localsend::to_json(request).dump());
  require(prepare.status == 200, "prepare upload chunked test failed");
  const auto response = localsend::prepare_upload_response_from_json(localsend::Json::parse(prepare.body));
  const std::string token = response.files.at(file.id);

  const std::string upload_path = std::string(localsend::kRouteUpload) + "?sessionId=" + response.session_id + "&fileId=" + file.id + "&token=" + token;
  const auto upload = localsend::http_post_chunked("127.0.0.1", server.port(), upload_path, chunks, file.file_type);
  require(upload.status == 200, "chunked upload failed");
  require(localsend::Json::parse(upload.body).is_object(), "chunked upload response should be JSON object");

  const auto received = dir / "chunked.txt";
  require(std::filesystem::exists(received), "chunked uploaded file missing");
  std::ifstream in(received, std::ios::binary);
  const std::string actual((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  require(actual == expected, "chunked uploaded file content mismatch");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_client_chunked_response() {
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(listen_fd >= 0, "chunked response listen socket failed");

  int enabled = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "chunked response bind failed");
  require(::listen(listen_fd, 1) == 0, "chunked response listen failed");

  socklen_t len = sizeof(addr);
  require(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "chunked response getsockname failed");
  const int port = ntohs(addr.sin_port);

  std::thread server([listen_fd]() {
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client >= 0) {
      char discard[1024];
      static_cast<void>(::recv(client, discard, sizeof(discard), 0));
      const char* response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Transfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n"
          "6\r\n{\"ok\":\r\n"
          "4\r\ntrue\r\n"
          "1\r\n}\r\n"
          "0\r\n\r\n";
      static_cast<void>(::send(client, response, std::strlen(response), 0));
      ::close(client);
    }
    ::close(listen_fd);
  });

  const auto response = localsend::http_get("127.0.0.1", port, "/chunked");
  server.join();

  require(response.status == 200, "chunked response status failed");
  require(response.body == R"({"ok":true})", "chunked response body failed");
}

} // namespace

int main() {
  try {
    test_json_round_trip();
    test_info_dto();
    test_prepare_upload_dto();
    test_prepare_upload_response_dto();
    test_multicast_dto();
    test_route_constants();
    test_default_config_paths();
    test_config_round_trip();
    test_safe_filename();
    test_unique_destination();
    test_http_server_routes_and_upload();
    test_http_send_multiple_files();
    test_http_v1_legacy_routes();
    test_http_cancel_rejects_upload();
    test_http_send_to_v1_target();
    test_http_prepare_uses_file_id();
    test_http_server_chunked_upload();
    test_http_client_chunked_response();
  } catch (const std::exception& e) {
    std::cerr << "test failed: " << e.what() << '\n';
    return 1;
  }

  std::cout << "core tests passed\n";
  return 0;
}
