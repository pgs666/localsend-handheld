#include "localsend/protocol.hpp"

#include "localsend/constants.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace localsend {
namespace {

std::string optional_string(const Json& json, const std::string& key, std::string fallback = {}) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return fallback;
  }
  return json.at(key).as_string();
}

bool optional_bool(const Json& json, const std::string& key, bool fallback = false) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return fallback;
  }
  return json.at(key).as_bool();
}

int optional_int(const Json& json, const std::string& key, int fallback) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return fallback;
  }
  return static_cast<int>(json.at(key).as_int64());
}

void put_if_not_empty(Json& json, const std::string& key, const std::string& value) {
  if (!value.empty()) {
    json[key] = value;
  }
}

} // namespace

std::string to_string(DeviceType value) {
  switch (value) {
  case DeviceType::Desktop:
    return "desktop";
  case DeviceType::Mobile:
    return "mobile";
  case DeviceType::Web:
    return "web";
  case DeviceType::Headless:
    return "headless";
  case DeviceType::Server:
    return "server";
  }
  return "desktop";
}

std::string to_string(ProtocolType value) {
  return value == ProtocolType::Https ? "https" : "http";
}

DeviceType device_type_from_string(const std::string& value) {
  if (value == "mobile") {
    return DeviceType::Mobile;
  }
  if (value == "web") {
    return DeviceType::Web;
  }
  if (value == "headless") {
    return DeviceType::Headless;
  }
  if (value == "server") {
    return DeviceType::Server;
  }
  return DeviceType::Desktop;
}

ProtocolType protocol_type_from_string(const std::string& value) {
  return value == "https" ? ProtocolType::Https : ProtocolType::Http;
}

Json to_json(const InfoDto& dto) {
  Json json = Json::object();
  json["alias"] = dto.alias;
  json["version"] = dto.version;
  json["deviceModel"] = dto.device_model;
  json["deviceType"] = to_string(dto.device_type);
  json["fingerprint"] = dto.fingerprint;
  json["download"] = dto.download;
  return json;
}

Json to_json(const InfoRegisterDto& dto) {
  Json json = to_json(static_cast<const InfoDto&>(dto));
  json["port"] = static_cast<std::int64_t>(dto.port);
  json["protocol"] = to_string(dto.protocol);
  return json;
}

Json to_json(const MulticastDto& dto) {
  Json json = to_json(static_cast<const InfoRegisterDto&>(dto));
  json["announce"] = dto.announce;
  return json;
}

Json to_json(const FileDto& dto) {
  Json json = Json::object();
  json["id"] = dto.id;
  json["fileName"] = dto.file_name;
  json["size"] = static_cast<std::int64_t>(dto.size);
  json["fileType"] = dto.file_type.empty() ? mime_from_filename(dto.file_name) : dto.file_type;
  put_if_not_empty(json, "hash", dto.hash);
  put_if_not_empty(json, "preview", dto.preview);
  return json;
}

Json to_json(const PrepareUploadRequestDto& dto) {
  Json files = Json::object();
  for (const auto& entry : dto.files) {
    files[entry.first] = to_json(entry.second);
  }

  Json json = Json::object();
  json["info"] = to_json(dto.info);
  json["files"] = std::move(files);
  return json;
}

Json to_json(const PrepareUploadResponseDto& dto) {
  Json files = Json::object();
  for (const auto& entry : dto.files) {
    files[entry.first] = entry.second;
  }

  Json json = Json::object();
  json["sessionId"] = dto.session_id;
  json["files"] = std::move(files);
  return json;
}

InfoDto info_from_json(const Json& json) {
  InfoDto dto;
  dto.alias = json.at("alias").as_string();
  dto.version = optional_string(json, "version", kProtocolVersion);
  dto.device_model = optional_string(json, "deviceModel", "Unknown");
  dto.device_type = device_type_from_string(optional_string(json, "deviceType", "desktop"));
  dto.fingerprint = optional_string(json, "fingerprint");
  dto.download = optional_bool(json, "download", false);
  return dto;
}

InfoRegisterDto info_register_from_json(const Json& json) {
  InfoRegisterDto dto;
  const InfoDto info = info_from_json(json);
  static_cast<InfoDto&>(dto) = info;
  dto.port = optional_int(json, "port", kDefaultPort);
  dto.protocol = protocol_type_from_string(optional_string(json, "protocol", "http"));
  return dto;
}

MulticastDto multicast_from_json(const Json& json) {
  MulticastDto dto;
  const InfoRegisterDto info = info_register_from_json(json);
  static_cast<InfoRegisterDto&>(dto) = info;
  dto.announce = optional_bool(json, "announce", true);
  return dto;
}

FileDto file_from_json(const Json& json) {
  FileDto dto;
  dto.id = json.at("id").as_string();
  dto.file_name = json.at("fileName").as_string();
  dto.size = static_cast<std::uint64_t>(json.at("size").as_int64());
  dto.file_type = optional_string(json, "fileType", mime_from_filename(dto.file_name));
  dto.hash = optional_string(json, "hash");
  dto.preview = optional_string(json, "preview");
  return dto;
}

PrepareUploadRequestDto prepare_upload_request_from_json(const Json& json) {
  PrepareUploadRequestDto dto;
  dto.info = info_register_from_json(json.at("info"));
  for (const auto& entry : json.at("files").as_object()) {
    dto.files.emplace(entry.first, file_from_json(entry.second));
  }
  return dto;
}

PrepareUploadResponseDto prepare_upload_response_from_json(const Json& json) {
  PrepareUploadResponseDto dto;
  dto.session_id = json.at("sessionId").as_string();
  for (const auto& entry : json.at("files").as_object()) {
    dto.files.emplace(entry.first, entry.second.as_string());
  }
  return dto;
}

std::string mime_from_filename(const std::string& filename) {
  std::filesystem::path path(filename);
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".flac") return "audio/flac";
  if (ext == ".txt" || ext == ".log") return "text/plain";
  if (ext == ".json") return "application/json";
  if (ext == ".pdf") return "application/pdf";
  if (ext == ".apk") return "application/vnd.android.package-archive";
  return "application/octet-stream";
}

} // namespace localsend

