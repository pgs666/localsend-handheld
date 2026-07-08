#include "localsend/config.hpp"

#include "localsend/constants.hpp"
#include "localsend/json.hpp"

#include <fstream>

namespace localsend {
namespace {

std::string platform_alias(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return "LocalSend Switch";
  case PlatformKind::Psv:
    return "LocalSend PSV";
  case PlatformKind::Desktop:
    return "LocalSend Desktop";
  }
  return "LocalSend Handheld";
}

std::filesystem::path default_inbox_path(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return "sdmc:/switch/localsend/inbox/";
  case PlatformKind::Psv:
    return "ux0:data/localsend/inbox/";
  case PlatformKind::Desktop:
    return "inbox/";
  }
  return "inbox/";
}

std::filesystem::path default_config_path(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return "sdmc:/switch/localsend/config.json";
  case PlatformKind::Psv:
    return "ux0:data/localsend/config.json";
  case PlatformKind::Desktop:
    return "local_config.json";
  }
  return "local_config.json";
}

std::filesystem::path default_certificate_path(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return "sdmc:/switch/localsend/cert.pem";
  case PlatformKind::Psv:
    return "ux0:data/localsend/cert.pem";
  case PlatformKind::Desktop:
    return "cert.pem";
  }
  return "cert.pem";
}

std::filesystem::path default_private_key_path(PlatformKind platform) {
  switch (platform) {
  case PlatformKind::Switch:
    return "sdmc:/switch/localsend/key.pem";
  case PlatformKind::Psv:
    return "ux0:data/localsend/key.pem";
  case PlatformKind::Desktop:
    return "key.pem";
  }
  return "key.pem";
}

std::string optional_string(const Json& json, const std::string& key, const std::string& fallback) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return fallback;
  }
  return json.at(key).as_string();
}

int optional_int(const Json& json, const std::string& key, int fallback) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return fallback;
  }
  return static_cast<int>(json.at(key).as_int64());
}

bool optional_bool(const Json& json, const std::string& key, bool fallback) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return fallback;
  }
  return json.at(key).as_bool();
}

Json to_json(const AppConfig& config) {
  Json json = Json::object();
  json["alias"] = config.alias;
  json["inboxPath"] = config.inbox_path.string();
  json["certificatePath"] = config.certificate_path.string();
  json["privateKeyPath"] = config.private_key_path.string();
  json["port"] = static_cast<std::int64_t>(config.port);
  json["discoveryEnabled"] = config.discovery_enabled;
  json["autoAccept"] = config.auto_accept;
  return json;
}

} // namespace

AppConfig default_config(PlatformKind platform) {
  AppConfig config;
  config.alias = platform_alias(platform);
  config.inbox_path = default_inbox_path(platform);
  config.config_path = default_config_path(platform);
  config.certificate_path = default_certificate_path(platform);
  config.private_key_path = default_private_key_path(platform);
  config.port = kDefaultPort;
  config.discovery_enabled = true;
  config.auto_accept = false;
  return config;
}

AppConfig load_config(PlatformKind platform, const std::filesystem::path& path) {
  AppConfig config = default_config(platform);
  config.config_path = path;

  std::ifstream in(path);
  if (!in) {
    return config;
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const Json json = Json::parse(text);
  config.alias = optional_string(json, "alias", config.alias);
  config.inbox_path = optional_string(json, "inboxPath", config.inbox_path.string());
  config.certificate_path = optional_string(json, "certificatePath", config.certificate_path.string());
  config.private_key_path = optional_string(json, "privateKeyPath", config.private_key_path.string());
  config.port = optional_int(json, "port", config.port);
  config.discovery_enabled = optional_bool(json, "discoveryEnabled", config.discovery_enabled);
  config.auto_accept = optional_bool(json, "autoAccept", config.auto_accept);
  return config;
}

void save_config(const AppConfig& config, const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  out << to_json(config).dump() << '\n';
}

} // namespace localsend
