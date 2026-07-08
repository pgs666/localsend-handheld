#include "localsend/constants.hpp"
#include "localsend/discovery.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"
#include "localsend/safe_path.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

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
  require(std::string(localsend::kDefaultMulticastGroup) == "224.0.0.167", "multicast group mismatch");
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
  {
    std::ofstream out(source, std::ios::binary);
    out << "hello localsend";
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
  std::string content;
  std::getline(in, content);
  require(content == "hello localsend", "uploaded file content mismatch");

  server.stop();
  std::filesystem::remove_all(dir);
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
    test_safe_filename();
    test_unique_destination();
    test_http_server_routes_and_upload();
  } catch (const std::exception& e) {
    std::cerr << "test failed: " << e.what() << '\n';
    return 1;
  }

  std::cout << "core tests passed\n";
  return 0;
}
