#pragma once

#include "localsend/json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace localsend {

enum class DeviceType {
  Desktop,
  Mobile,
  Web,
  Headless,
  Server,
};

enum class ProtocolType {
  Http,
  Https,
};

struct Device {
  std::string ip;
  std::string version = "2.1";
  int port = 53317;
  bool https = false;
  std::string fingerprint;
  std::string alias;
  std::string device_model;
  DeviceType device_type = DeviceType::Desktop;
  bool download = false;
};

struct InfoDto {
  std::string alias;
  std::string version = "2.1";
  std::string device_model = "LocalSend Handheld";
  DeviceType device_type = DeviceType::Desktop;
  std::string fingerprint;
  bool download = false;
};

struct InfoRegisterDto : InfoDto {
  int port = 53317;
  ProtocolType protocol = ProtocolType::Http;
};

struct MulticastDto : InfoRegisterDto {
  bool announce = true;
};

struct FileDto {
  std::string id;
  std::string file_name;
  std::uint64_t size = 0;
  std::string file_type = "application/octet-stream";
  std::string hash;
  std::string preview;
};

struct PrepareUploadRequestDto {
  InfoRegisterDto info;
  std::map<std::string, FileDto> files;
};

struct PrepareUploadResponseDto {
  std::string session_id;
  std::map<std::string, std::string> files;
};

std::string to_string(DeviceType value);
std::string to_string(ProtocolType value);
DeviceType device_type_from_string(const std::string& value);
ProtocolType protocol_type_from_string(const std::string& value);

Json to_json(const InfoDto& dto);
Json to_json(const InfoRegisterDto& dto);
Json to_json(const MulticastDto& dto);
Json to_json(const FileDto& dto);
Json to_json(const PrepareUploadRequestDto& dto);
Json to_json(const PrepareUploadResponseDto& dto);

InfoDto info_from_json(const Json& json);
InfoRegisterDto info_register_from_json(const Json& json);
MulticastDto multicast_from_json(const Json& json);
FileDto file_from_json(const Json& json);
PrepareUploadRequestDto prepare_upload_request_from_json(const Json& json);
PrepareUploadResponseDto prepare_upload_response_from_json(const Json& json);

std::string mime_from_filename(const std::string& filename);

} // namespace localsend

