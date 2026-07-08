#include "localsend/app_service.hpp"
#include "localsend/constants.hpp"
#include "localsend/config.hpp"
#include "localsend/device_store.hpp"
#include "localsend/device_selection.hpp"
#include "localsend/discovery.hpp"
#include "localsend/file_browser.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"
#include "localsend/safe_path.hpp"
#include "localsend/security.hpp"
#include "localsend/status_format.hpp"
#include "localsend/tls.hpp"
#include "localsend/transfer.hpp"

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

#if LOCALSEND_HAS_MBEDTLS
#include <mbedtls/version.h>
#endif

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
  const auto json = localsend::Json::parse(payload);
  require(json.at("announce").as_bool(), "multicast announce field missing");
  require(json.at("announcement").as_bool(), "multicast announcement compat field missing");
  const auto device = localsend::device_from_multicast(payload, "192.168.1.50", 53317);
  require(device.ip == "192.168.1.50", "multicast ip failed");
  require(device.alias == "Switch", "multicast alias failed");
  require(device.port == 53317, "multicast port failed");
  require(!device.https, "multicast protocol failed");
}

void test_transfer_store_lifecycle() {
  localsend::TransferStore store;
  const auto receive_id = store.add(localsend::TransferDirection::Receive,
                                    "photo.jpg",
                                    100,
                                    "Phone",
                                    "192.168.1.20");
  const auto send_id = store.add(localsend::TransferDirection::Send,
                                 "save.zip",
                                 20,
                                 "Desktop",
                                 "192.168.1.30");

  require(receive_id != send_id, "transfer ids should be unique");
  require(store.snapshot().size() == 2, "transfer snapshot size failed");

  auto receive = store.get(receive_id);
  require(receive.has_value(), "transfer get failed");
  require(receive->status == localsend::TransferStatus::Pending, "new transfer should be pending");
  require(receive->direction == localsend::TransferDirection::Receive, "transfer direction failed");
  require(receive->file_name == "photo.jpg", "transfer file name failed");
  require(receive->peer_alias == "Phone", "transfer peer alias failed");

  require(store.set_status(receive_id, localsend::TransferStatus::Preparing), "transfer preparing failed");
  require(store.set_progress(receive_id, 40), "transfer progress failed");
  receive = store.get(receive_id);
  require(receive->status == localsend::TransferStatus::Transferring, "transfer progress should mark transferring");
  require(receive->bytes_transferred == 40, "transfer bytes failed");

  require(store.set_progress(receive_id, 500), "transfer completed progress failed");
  receive = store.get(receive_id);
  require(receive->status == localsend::TransferStatus::Completed, "transfer should complete at full size");
  require(receive->bytes_transferred == 100, "transfer progress should clamp to size");

  require(store.fail(send_id, "network error"), "transfer fail failed");
  auto send = store.get(send_id);
  require(send->status == localsend::TransferStatus::Failed, "failed transfer status failed");
  require(send->error == "network error", "failed transfer error failed");

  store.clear_completed();
  require(store.snapshot().empty(), "clear completed should remove terminal transfers");
}

void test_transfer_store_cancel_and_strings() {
  localsend::TransferStore store;
  const auto id = store.add(localsend::TransferDirection::Send, "demo.bin", 8, "Peer", "127.0.0.1");

  require(std::string(localsend::to_string(localsend::TransferDirection::Send)) == "send", "send direction string failed");
  require(std::string(localsend::to_string(localsend::TransferDirection::Receive)) == "receive", "receive direction string failed");
  require(std::string(localsend::to_string(localsend::TransferStatus::Pending)) == "pending", "pending status string failed");
  require(std::string(localsend::to_string(localsend::TransferStatus::Preparing)) == "preparing", "preparing status string failed");
  require(std::string(localsend::to_string(localsend::TransferStatus::Transferring)) == "transferring", "transferring status string failed");
  require(std::string(localsend::to_string(localsend::TransferStatus::Completed)) == "completed", "completed status string failed");
  require(std::string(localsend::to_string(localsend::TransferStatus::Failed)) == "failed", "failed status string failed");
  require(std::string(localsend::to_string(localsend::TransferStatus::Cancelled)) == "cancelled", "cancelled status string failed");

  require(store.cancel(id), "transfer cancel failed");
  const auto item = store.get(id);
  require(item.has_value(), "cancelled transfer missing");
  require(item->status == localsend::TransferStatus::Cancelled, "cancelled transfer status failed");
  require(!store.cancel(9999), "unknown transfer cancel should fail");

  store.clear();
  require(store.snapshot().empty(), "transfer clear failed");
}

void test_device_store_upsert_and_sources() {
  localsend::DeviceStore store;

  localsend::Device switch_device;
  switch_device.ip = "192.168.1.50";
  switch_device.port = 53317;
  switch_device.alias = "Switch";
  switch_device.https = true;
  switch_device.fingerprint = "abc123";

  const std::string first_key = store.upsert_discovered(switch_device);
  require(first_key == "fingerprint:abc123", "device fingerprint key failed");
  require(store.snapshot().size() == 1, "device store insert failed");

  switch_device.alias = "LocalSend Switch";
  switch_device.ip = "192.168.1.51";
  const std::string second_key = store.upsert_discovered(switch_device);
  require(second_key == first_key, "device fingerprint should keep stable key");
  require(store.snapshot().size() == 1, "device upsert should not duplicate");

  auto entry = store.get(first_key);
  require(entry.has_value(), "device get failed");
  require(entry->device.alias == "LocalSend Switch", "device update alias failed");
  require(entry->device.ip == "192.168.1.51", "device update ip failed");
  require(entry->source == localsend::DeviceSource::Discovered, "device source failed");
  require(entry->online, "updated device should be online");

  localsend::Device manual;
  manual.ip = "192.168.1.70";
  manual.port = 53317;
  manual.alias = "Manual peer";
  const std::string manual_key = store.upsert_manual(manual);
  require(manual_key == "endpoint:192.168.1.70:53317", "manual endpoint key failed");
  require(store.snapshot().size() == 2, "manual device insert failed");
  require(store.get(manual_key)->source == localsend::DeviceSource::Manual, "manual source failed");

  require(std::string(localsend::to_string(localsend::DeviceSource::Discovered)) == "discovered", "discovered source string failed");
  require(std::string(localsend::to_string(localsend::DeviceSource::Manual)) == "manual", "manual source string failed");
}

void test_device_store_offline_remove_and_clear() {
  localsend::DeviceStore store;

  localsend::Device device;
  device.ip = "10.0.0.2";
  device.port = 53317;
  device.alias = "Desktop";
  const std::string key = store.upsert_discovered(device);

  require(store.mark_offline(key), "device mark offline failed");
  auto entry = store.get(key);
  require(entry.has_value(), "offline device missing");
  require(!entry->online, "device should be offline");

  require(store.upsert_discovered(device) == key, "device re-upsert key failed");
  entry = store.get(key);
  require(entry->online, "device refresh should mark online");

  localsend::Device manual;
  manual.ip = "10.0.0.3";
  manual.port = 53317;
  manual.alias = "Manual";
  const std::string manual_key = store.upsert_manual(manual);

  require(store.mark_stale_offline(std::chrono::seconds(-1)) == 1, "stale device count failed");
  entry = store.get(key);
  require(!entry->online, "stale device should be offline");
  const auto manual_entry = store.get(manual_key);
  require(manual_entry.has_value(), "manual stale device missing");
  require(manual_entry->online, "manual device should not be marked stale");

  require(store.remove(key), "device remove failed");
  require(!store.get(key).has_value(), "removed device should be missing");
  require(!store.remove(key), "removing unknown device should fail");

  store.upsert_discovered(device);
  store.clear();
  require(store.snapshot().empty(), "device clear failed");
}

void test_status_format_summaries() {
  std::vector<localsend::DeviceEntry> devices;
  localsend::DeviceEntry device;
  device.key = "endpoint:192.168.1.10:53317";
  device.device.ip = "192.168.1.10";
  device.device.port = 53317;
  device.device.alias = "MateBook";
  device.device.https = true;
  device.source = localsend::DeviceSource::Manual;
  device.online = true;
  devices.push_back(device);

  const std::string device_summary = localsend::format_device_summary(devices);
  require(device_summary.find("online MateBook [https] 192.168.1.10:53317 (manual)") != std::string::npos,
          "device summary failed");
  require(localsend::format_device_summary({}) == "No peers yet", "empty device summary failed");

  std::vector<localsend::TransferItem> transfers;
  localsend::TransferItem old_item;
  old_item.direction = localsend::TransferDirection::Receive;
  old_item.status = localsend::TransferStatus::Completed;
  old_item.file_name = "old.bin";
  old_item.size = 4;
  old_item.bytes_transferred = 4;
  old_item.peer_alias = "Phone";
  transfers.push_back(old_item);

  localsend::TransferItem item;
  item.direction = localsend::TransferDirection::Send;
  item.status = localsend::TransferStatus::Transferring;
  item.file_name = "demo.bin";
  item.size = 100;
  item.bytes_transferred = 40;
  item.peer_alias = "MateBook";
  transfers.push_back(item);

  const std::string transfer_summary = localsend::format_transfer_summary(transfers, 1);
  require(transfer_summary.find("send demo.bin -> MateBook [transferring, 40%]") != std::string::npos,
          "transfer summary failed");
  require(transfer_summary.find("+1 older") != std::string::npos, "transfer summary overflow failed");
  require(localsend::format_transfer_summary({}) == "No transfers yet", "empty transfer summary failed");
}

void test_device_selection_online_cycle() {
  std::vector<localsend::DeviceEntry> devices;

  localsend::DeviceEntry offline;
  offline.key = "offline";
  offline.device.alias = "Offline";
  offline.device.ip = "192.168.1.2";
  offline.device.port = 53317;
  offline.online = false;
  devices.push_back(offline);

  localsend::DeviceEntry first;
  first.key = "first";
  first.device.alias = "First";
  first.device.ip = "192.168.1.3";
  first.device.port = 53317;
  first.online = true;
  devices.push_back(first);

  localsend::DeviceEntry second;
  second.key = "second";
  second.device.alias = "Second";
  second.device.ip = "192.168.1.4";
  second.device.port = 53318;
  second.device.https = true;
  second.online = true;
  devices.push_back(second);

  const auto first_index = localsend::first_online_device_index(devices);
  require(first_index.has_value() && *first_index == 1, "first online device failed");

  const auto next_index = localsend::next_online_device_index(devices, first_index);
  require(next_index.has_value() && *next_index == 2, "next online device failed");

  const auto wrapped_index = localsend::next_online_device_index(devices, next_index);
  require(wrapped_index.has_value() && *wrapped_index == 1, "next online wrap failed");

  const auto selected = localsend::selected_online_device(devices, 0);
  require(selected.has_value() && selected->key == "first", "selected online fallback failed");

  const std::string formatted = localsend::format_selected_device(devices, next_index);
  require(formatted == "Second [https] 192.168.1.4:53318", "selected device format failed");

  devices[1].online = false;
  devices[2].online = false;
  require(!localsend::first_online_device_index(devices).has_value(), "no online first should be empty");
  require(localsend::format_selected_device(devices, std::nullopt) == "No online peer selected",
          "no selected device format failed");
}

void test_security_fingerprint() {
  const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
  require(localsend::sha256_hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 test vector failed");

  const std::string certificate = R"(-----BEGIN CERTIFICATE-----
MIIDEzCCAfugAwIBAgIUY6htbFfRtc1NPzwqLcjgnnNtajgwDQYJKoZIhvcNAQEL
BQAwGTEXMBUGA1UEAwwOTG9jYWxTZW5kIFVzZXIwHhcNMjYwNzA4MDQyOTE0WhcN
MzYwNzA1MDQyOTE0WjAZMRcwFQYDVQQDDA5Mb2NhbFNlbmQgVXNlcjCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAMYGYIfA2Lclrmnmi8UpAoTVUgki2KkV
6LyVvRqMexc4u2rwq/t31/CFNCeSadJm3l3xrol/eWyqJiylwLbh3c0x9DaC+k8X
dC5LKlAlWzY9HV6zN656QPKG1k5KxROoDW+XsVwWE2pIjmK5ejvp7lCr9xLPHgab
02++1nb69FLkwu7bOs0Njt1MWX1FHCEBQOHNkYB5XXXknXbuYEw58zgow80mqnKr
YVZQuWTa3/mQvHdRSfhmsca3pTUrg93Ip7Ywpdy/SWEMuBotIT7XPCU86ifiNubv
3KyQJWB1tezn5hFIVpQSL5cjprUQvijMFWZeKMOvk+iBn435dJBHpgsCAwEAAaNT
MFEwHQYDVR0OBBYEFNak+IojlJDpn8xHcespg7x2al1SMB8GA1UdIwQYMBaAFNak
+IojlJDpn8xHcespg7x2al1SMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL
BQADggEBAGHuAcMxWZ3JL4nu4h2OS6/ywKZ/gLxEUGvTJs3EVF4bKy2Od+qds/DT
+db3jq3/iZgGesQaJ7FTL4eTgsClUVxTx7anJYm+Bi2xCLacGOQyb+O5XdBn57Qk
rboFbidmAFody+K3aJtTYu3UldIR1CKPxz7vTXQqytrvRnsS3x+OpQwG6fsJyCG4
QDiQ90W/IhNtLdUxz06JVXoBz7lumUAfvWOwdrnAmxBSWxi3mLyOxgSZUUjzKc1e
UKEcKLl7Dcx7K6HHmbHHOPuuNG+oj+p5cz51IEQX1FM3FiR754vLc0mlC0dvolie
VCUMCMmX5tNBCQ8q4QG99msObKjrj3o=
-----END CERTIFICATE-----)";
  require(localsend::certificate_der_from_pem(certificate).size() > 0, "certificate DER extraction failed");
  require(localsend::certificate_fingerprint_from_pem(certificate) == "4ced1a640a8d0b577eb2b79537957153643d387e2c74f8ceadb3424ce64e6954",
          "certificate fingerprint failed");
}

void test_tls_identity_persistence() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-identity-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto certificate_path = dir / "cert.pem";
  const auto private_key_path = dir / "key.pem";

  const auto created = localsend::load_or_create_tls_identity(certificate_path, private_key_path);
  require(std::filesystem::exists(certificate_path), "generated certificate missing");
  require(std::filesystem::exists(private_key_path), "generated private key missing");
  require(created.certificate_pem.find("BEGIN CERTIFICATE") != std::string::npos, "generated certificate PEM invalid");
  require(created.private_key_pem.find("BEGIN") != std::string::npos, "generated private key PEM invalid");
  require(created.fingerprint.size() == 64, "generated fingerprint length failed");
  require(created.fingerprint == localsend::certificate_fingerprint_from_pem(created.certificate_pem), "generated fingerprint mismatch");

  const auto loaded = localsend::load_or_create_tls_identity(certificate_path, private_key_path);
  require(loaded.certificate_pem == created.certificate_pem, "TLS identity certificate should be reused");
  require(loaded.private_key_pem == created.private_key_pem, "TLS identity private key should be reused");
  require(loaded.fingerprint == created.fingerprint, "TLS identity fingerprint should be stable");

  localsend::InfoRegisterDto self;
  self.alias = "Generated TLS Receiver";
  self.protocol = localsend::ProtocolType::Https;
  self.fingerprint = created.fingerprint;

  const auto inbox = dir / "inbox";
  localsend::LocalSendServer server(self, inbox, {created.certificate_pem, created.private_key_pem});
  require(server.start(0), "generated TLS identity server failed to start");
  const auto info = localsend::https_get("127.0.0.1", server.port(), localsend::kRouteInfo, created.fingerprint);
  require(info.status == 200, "generated TLS identity info failed");
  server.stop();

  std::filesystem::remove_all(dir);
}

void test_mbedtls_linked() {
#if LOCALSEND_HAS_MBEDTLS
  require(mbedtls_version_get_number() >= 0x03060700, "mbedTLS version is older than pinned release");
#else
  require(false, "mbedTLS should be enabled");
#endif
}

const std::string& test_tls_certificate() {
  static const std::string certificate = R"(-----BEGIN CERTIFICATE-----
MIIDEzCCAfugAwIBAgIUZAO97Ffzy9Gp+mEPoGoUsNQG6bUwDQYJKoZIhvcNAQEL
BQAwGTEXMBUGA1UEAwwOTG9jYWxTZW5kIFVzZXIwHhcNMjYwNzA4MDQ0MTMwWhcN
MzYwNzA1MDQ0MTMwWjAZMRcwFQYDVQQDDA5Mb2NhbFNlbmQgVXNlcjCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAM+nwggiabq3hjx+saaPohPLJEUg1Jnx
4iQ5c0Z7T+zH7AkXrQwoR7RkZNMYaPXx7qTlDQxe0WEDexbtoeKCSDB3u3/4GkOL
G0YgNeVrxGxN8DuaRleMRxS/Z1VoVPaZadNs0zjp931V5Zm2dFrnocERDjX6iWdZ
lv9g/SF1EU0hdbprZSKrZSFt9ZCgXIDCnb/1MPle5yW1CuT4y6s9RVzFVJflKPU5
4t0SIB0sVWFxqKKU207nBUfCldqyn+CUll+OsOFtmgsSrvV8QwOVLylIwKCi2Njj
/mfIrYyNkDSP0IalQwln3VPdi3dUbgYw5WMed/rd7rYn4QhjRTvi2VECAwEAAaNT
MFEwHQYDVR0OBBYEFHlwJYjjkjW5OPAsdyTE7w6cCzvcMB8GA1UdIwQYMBaAFHlw
JYjjkjW5OPAsdyTE7w6cCzvcMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL
BQADggEBADpNbCblsdB9y2nvMEz4wTQambJQvYfTgPBOYgPa5nQxGc1wbFHiBr13
QRzN/uNATcUNKtmJve3br/sea4Jr10VAX50pdrE+6v4c572UCG2/JrOu+k1fT+55
iiaf1AR5FwU/MRC9eBESoOXy3JXsM8S9ZILfIqmfV+ca+S53rhhrrYjedRnLJkS0
IN3Kvbzjy0o8VRMxCGuwB4iAgjiwpvNL5JZSw2rhf+C1KiGhjA+ldD3qonIxekMK
zPo1oE1CUoAI2vUUEG0egmZ+MB9KFd5f09PZGcYOwmBLjU4/clJAnj4924axxVAd
7XYy3eHGG4xk8GDyn7+fTaw2fgdBR+c=
-----END CERTIFICATE-----)";
  return certificate;
}

const std::string& test_tls_private_key() {
  static const std::string private_key = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDPp8IIImm6t4Y8
frGmj6ITyyRFINSZ8eIkOXNGe0/sx+wJF60MKEe0ZGTTGGj18e6k5Q0MXtFhA3sW
7aHigkgwd7t/+BpDixtGIDXla8RsTfA7mkZXjEcUv2dVaFT2mWnTbNM46fd9VeWZ
tnRa56HBEQ41+olnWZb/YP0hdRFNIXW6a2Uiq2UhbfWQoFyAwp2/9TD5XucltQrk
+MurPUVcxVSX5Sj1OeLdEiAdLFVhcaiilNtO5wVHwpXasp/glJZfjrDhbZoLEq71
fEMDlS8pSMCgotjY4/5nyK2MjZA0j9CGpUMJZ91T3Yt3VG4GMOVjHnf63e62J+EI
Y0U74tlRAgMBAAECggEAM9vygjON8hqJRKxjU3SFhqnx6e20Cqo0ztUmK9D5+elH
0lF+Xw3kMnHsGCf9doawEbA+XPuFENRctjIsfrQIsUoFooTkkj+4VQAQVbZfPKkO
OORjctPOoKjYdqTyqw9PNYT1Dz6nFz8Pcx702gsFA4Ft6h8il5PxOOAQ930UEA26
d7xsfSzBREqt94SwdXkmXTrPdb9DQJh5+mve6sUvvVJg2OXtncwW18q4dsh3LONn
ATLsQXc5tFOPAXu5fihfmT4fBLEyZ5fNigfmxY3paGNKtXY4ChJNay8SVBR34mgW
akxziAqqSc0uJHLcNpkiLOFM5nvON/fYW0hS2r1tjQKBgQD9r/60ipcYhP4m263n
bPiYip0lFhkdAy8vc4BTMRCu6qPhy7AhKFwSxM2AkHREtkMp7CDO44dI5sREFAWy
w7SMjxGIHl+oNaUu6vWGWP7rU9OR7t5HmgUWxdryjy17/rssTLW+FCiuOAWRrS+h
9aCnLNPJTwNDeXd51oeDuBGhewKBgQDRjFeiGdXKPZ7WKiL2DWXjQs7stMKLRmR4
R4nvw0uYpK5nkB33napz5ECNceU41G8kUAkiM5r/cJlppNkE66uA1aOl8Q1NRo64
ViUPgsT0YygFz+Tb0YR9p9VWdhDzVZkxJdK1/ExFQ9y7RwX1OanF5keGthqRZrtQ
y2XRo9iYowKBgCW212fhvqq/gsUmHYltMtwCp3APA/bDNW2Zfzde8PsAGRMFZA7Z
4C5OIbr+PrrEWeHOn+YB/2fAHud8DojP/XR0BIg288OfDgqWlZ++dU9o6+gjGdqN
NDp5eZ5b2Mg5S3w/fzld59pWq8VHePBcAuE3kdi4rWSHl1J+qTDU2ZInAoGAcTXg
Voycq2H1QYGMV+DPLiP3BX13KaXDPBRyWl3pprM6ImuDNTcyUuB7W6+wBq8GyNiQ
xrCYye68g43zTaxBgR5rBokgBaLcEo1AAoxE+j/j7Jfv7i7Y5MZbBRZOfBi/5gSo
PXfsgPNz+p4Zgu4/YdLSy93wpqOZCcKJ5OQfbf8CgYEApBY3ilFEE0Zw80bFX20N
qoLDwY7lxiCjGQ5UcIVkfHEAzY82IdBuNnL3Jt4S+wcbhupM6/tCDszGdUc9k6wQ
K+TP1E/eXjTp2xnYhSTFy4KrQA+qOMWPMrS1oYJcPH/i8S690DVTJawSpva3qgtL
BdqGLvSTqbINqoRmcdIP1Qo=
-----END PRIVATE KEY-----)";
  return private_key;
}

void test_tls_loopback() {
  const std::string certificate = R"(-----BEGIN CERTIFICATE-----
MIIDEzCCAfugAwIBAgIUZAO97Ffzy9Gp+mEPoGoUsNQG6bUwDQYJKoZIhvcNAQEL
BQAwGTEXMBUGA1UEAwwOTG9jYWxTZW5kIFVzZXIwHhcNMjYwNzA4MDQ0MTMwWhcN
MzYwNzA1MDQ0MTMwWjAZMRcwFQYDVQQDDA5Mb2NhbFNlbmQgVXNlcjCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAM+nwggiabq3hjx+saaPohPLJEUg1Jnx
4iQ5c0Z7T+zH7AkXrQwoR7RkZNMYaPXx7qTlDQxe0WEDexbtoeKCSDB3u3/4GkOL
G0YgNeVrxGxN8DuaRleMRxS/Z1VoVPaZadNs0zjp931V5Zm2dFrnocERDjX6iWdZ
lv9g/SF1EU0hdbprZSKrZSFt9ZCgXIDCnb/1MPle5yW1CuT4y6s9RVzFVJflKPU5
4t0SIB0sVWFxqKKU207nBUfCldqyn+CUll+OsOFtmgsSrvV8QwOVLylIwKCi2Njj
/mfIrYyNkDSP0IalQwln3VPdi3dUbgYw5WMed/rd7rYn4QhjRTvi2VECAwEAAaNT
MFEwHQYDVR0OBBYEFHlwJYjjkjW5OPAsdyTE7w6cCzvcMB8GA1UdIwQYMBaAFHlw
JYjjkjW5OPAsdyTE7w6cCzvcMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL
BQADggEBADpNbCblsdB9y2nvMEz4wTQambJQvYfTgPBOYgPa5nQxGc1wbFHiBr13
QRzN/uNATcUNKtmJve3br/sea4Jr10VAX50pdrE+6v4c572UCG2/JrOu+k1fT+55
iiaf1AR5FwU/MRC9eBESoOXy3JXsM8S9ZILfIqmfV+ca+S53rhhrrYjedRnLJkS0
IN3Kvbzjy0o8VRMxCGuwB4iAgjiwpvNL5JZSw2rhf+C1KiGhjA+ldD3qonIxekMK
zPo1oE1CUoAI2vUUEG0egmZ+MB9KFd5f09PZGcYOwmBLjU4/clJAnj4924axxVAd
7XYy3eHGG4xk8GDyn7+fTaw2fgdBR+c=
-----END CERTIFICATE-----)";
  const std::string private_key = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDPp8IIImm6t4Y8
frGmj6ITyyRFINSZ8eIkOXNGe0/sx+wJF60MKEe0ZGTTGGj18e6k5Q0MXtFhA3sW
7aHigkgwd7t/+BpDixtGIDXla8RsTfA7mkZXjEcUv2dVaFT2mWnTbNM46fd9VeWZ
tnRa56HBEQ41+olnWZb/YP0hdRFNIXW6a2Uiq2UhbfWQoFyAwp2/9TD5XucltQrk
+MurPUVcxVSX5Sj1OeLdEiAdLFVhcaiilNtO5wVHwpXasp/glJZfjrDhbZoLEq71
fEMDlS8pSMCgotjY4/5nyK2MjZA0j9CGpUMJZ91T3Yt3VG4GMOVjHnf63e62J+EI
Y0U74tlRAgMBAAECggEAM9vygjON8hqJRKxjU3SFhqnx6e20Cqo0ztUmK9D5+elH
0lF+Xw3kMnHsGCf9doawEbA+XPuFENRctjIsfrQIsUoFooTkkj+4VQAQVbZfPKkO
OORjctPOoKjYdqTyqw9PNYT1Dz6nFz8Pcx702gsFA4Ft6h8il5PxOOAQ930UEA26
d7xsfSzBREqt94SwdXkmXTrPdb9DQJh5+mve6sUvvVJg2OXtncwW18q4dsh3LONn
ATLsQXc5tFOPAXu5fihfmT4fBLEyZ5fNigfmxY3paGNKtXY4ChJNay8SVBR34mgW
akxziAqqSc0uJHLcNpkiLOFM5nvON/fYW0hS2r1tjQKBgQD9r/60ipcYhP4m263n
bPiYip0lFhkdAy8vc4BTMRCu6qPhy7AhKFwSxM2AkHREtkMp7CDO44dI5sREFAWy
w7SMjxGIHl+oNaUu6vWGWP7rU9OR7t5HmgUWxdryjy17/rssTLW+FCiuOAWRrS+h
9aCnLNPJTwNDeXd51oeDuBGhewKBgQDRjFeiGdXKPZ7WKiL2DWXjQs7stMKLRmR4
R4nvw0uYpK5nkB33napz5ECNceU41G8kUAkiM5r/cJlppNkE66uA1aOl8Q1NRo64
ViUPgsT0YygFz+Tb0YR9p9VWdhDzVZkxJdK1/ExFQ9y7RwX1OanF5keGthqRZrtQ
y2XRo9iYowKBgCW212fhvqq/gsUmHYltMtwCp3APA/bDNW2Zfzde8PsAGRMFZA7Z
4C5OIbr+PrrEWeHOn+YB/2fAHud8DojP/XR0BIg288OfDgqWlZ++dU9o6+gjGdqN
NDp5eZ5b2Mg5S3w/fzld59pWq8VHePBcAuE3kdi4rWSHl1J+qTDU2ZInAoGAcTXg
Voycq2H1QYGMV+DPLiP3BX13KaXDPBRyWl3pprM6ImuDNTcyUuB7W6+wBq8GyNiQ
xrCYye68g43zTaxBgR5rBokgBaLcEo1AAoxE+j/j7Jfv7i7Y5MZbBRZOfBi/5gSo
PXfsgPNz+p4Zgu4/YdLSy93wpqOZCcKJ5OQfbf8CgYEApBY3ilFEE0Zw80bFX20N
qoLDwY7lxiCjGQ5UcIVkfHEAzY82IdBuNnL3Jt4S+wcbhupM6/tCDszGdUc9k6wQ
K+TP1E/eXjTp2xnYhSTFy4KrQA+qOMWPMrS1oYJcPH/i8S690DVTJawSpva3qgtL
BdqGLvSTqbINqoRmcdIP1Qo=
-----END PRIVATE KEY-----)";
  const std::string expected_fingerprint = "0078271eba40de56b3c0701a406a0d8a1dc430cd158af1899c6820fdd6a6964c";

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(listen_fd >= 0, "tls listen socket failed");
  int enabled = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "tls bind failed");
  require(::listen(listen_fd, 1) == 0, "tls listen failed");
  socklen_t len = sizeof(addr);
  require(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "tls getsockname failed");
  const int port = ntohs(addr.sin_port);

  std::thread server([&]() {
    const int client_fd = ::accept(listen_fd, nullptr, nullptr);
    require(client_fd >= 0, "tls accept failed");
    auto tls = localsend::TlsConnection::server(client_fd, {certificate, private_key});
    require(tls.handshake(), "server TLS handshake failed");
    std::uint8_t buffer[16] = {};
    const int read = tls.read(buffer, sizeof(buffer));
    require(read == 4 && std::string(reinterpret_cast<char*>(buffer), 4) == "ping", "server TLS read failed");
    const std::string pong = "pong";
    require(tls.write_all(reinterpret_cast<const std::uint8_t*>(pong.data()), pong.size()), "server TLS write failed");
    tls.close_notify();
    ::close(listen_fd);
  });

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "tls client socket failed");
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  target.sin_port = htons(static_cast<uint16_t>(port));
  require(::connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0, "tls client connect failed");
  auto tls = localsend::TlsConnection::client(fd);
  require(tls.handshake(), "client TLS handshake failed");
  require(tls.peer_fingerprint() == expected_fingerprint, "client peer fingerprint failed");
  const std::string ping = "ping";
  require(tls.write_all(reinterpret_cast<const std::uint8_t*>(ping.data()), ping.size()), "client TLS write failed");
  std::uint8_t buffer[16] = {};
  const int read = tls.read(buffer, sizeof(buffer));
  require(read == 4 && std::string(reinterpret_cast<char*>(buffer), 4) == "pong", "client TLS read failed");
  tls.close_notify();
  server.join();
}

void test_route_constants() {
  require(std::string(localsend::kRouteInfo) == "/api/localsend/v2/info", "info route mismatch");
  require(std::string(localsend::kRoutePrepareUpload) == "/api/localsend/v2/prepare-upload", "prepare route mismatch");
  require(std::string(localsend::kRouteInfoV1) == "/api/localsend/v1/info", "v1 info route mismatch");
  require(std::string(localsend::kRoutePrepareUploadV1) == "/api/localsend/v1/send-request", "v1 prepare route mismatch");
  require(std::string(localsend::kDefaultMulticastGroup) == "224.0.0.167", "multicast group mismatch");
  require(std::string(localsend::kDefaultBroadcastAddress) == "255.255.255.255", "broadcast address mismatch");
}

void test_default_config_paths() {
  const auto switch_config = localsend::default_config(localsend::PlatformKind::Switch);
  require(switch_config.inbox_path.string() == "sdmc:/switch/localsend/inbox/", "switch inbox path mismatch");
  require(switch_config.outbox_path.string() == "sdmc:/switch/localsend/outbox/", "switch outbox path mismatch");
  require(switch_config.config_path.string() == "sdmc:/switch/localsend/config.json", "switch config path mismatch");
  require(switch_config.certificate_path.string() == "sdmc:/switch/localsend/cert.pem", "switch certificate path mismatch");
  require(switch_config.private_key_path.string() == "sdmc:/switch/localsend/key.pem", "switch private key path mismatch");

  const auto psv_config = localsend::default_config(localsend::PlatformKind::Psv);
  require(psv_config.inbox_path.string() == "ux0:data/localsend/inbox/", "psv inbox path mismatch");
  require(psv_config.outbox_path.string() == "ux0:data/localsend/outbox/", "psv outbox path mismatch");
  require(psv_config.config_path.string() == "ux0:data/localsend/config.json", "psv config path mismatch");
  require(psv_config.certificate_path.string() == "ux0:data/localsend/cert.pem", "psv certificate path mismatch");
  require(psv_config.private_key_path.string() == "ux0:data/localsend/key.pem", "psv private key path mismatch");
}

void test_config_round_trip() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-config-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "config.json";

  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Desk";
  config.inbox_path = "downloads";
  config.outbox_path = "uploads";
  config.certificate_path = "tls/cert.pem";
  config.private_key_path = "tls/key.pem";
  config.port = 12345;
  config.discovery_enabled = false;
  config.auto_accept = true;
  config.manual_devices.push_back({"192.168.1.10", 53317, false, "Matebook", ""});
  config.manual_devices.push_back({"192.168.1.11", 53318, true, "Phone", "phone-fingerprint"});
  localsend::save_config(config, path);

  const auto loaded = localsend::load_config(localsend::PlatformKind::Desktop, path);
  require(loaded.alias == "Desk", "config alias round trip failed");
  require(loaded.inbox_path.string() == "downloads", "config inbox round trip failed");
  require(loaded.outbox_path.string() == "uploads", "config outbox round trip failed");
  require(loaded.certificate_path.string() == "tls/cert.pem", "config certificate round trip failed");
  require(loaded.private_key_path.string() == "tls/key.pem", "config private key round trip failed");
  require(loaded.port == 12345, "config port round trip failed");
  require(!loaded.discovery_enabled, "config discovery round trip failed");
  require(loaded.auto_accept, "config auto accept round trip failed");
  require(loaded.manual_devices.size() == 2, "config manual device count failed");
  require(loaded.manual_devices[0].ip == "192.168.1.10", "config manual device ip failed");
  require(loaded.manual_devices[0].port == 53317, "config manual device port failed");
  require(!loaded.manual_devices[0].https, "config manual device protocol failed");
  require(loaded.manual_devices[0].alias == "Matebook", "config manual device alias failed");
  require(loaded.manual_devices[1].ip == "192.168.1.11", "config second manual device ip failed");
  require(loaded.manual_devices[1].port == 53318, "config second manual device port failed");
  require(loaded.manual_devices[1].https, "config second manual device protocol failed");
  require(loaded.manual_devices[1].fingerprint == "phone-fingerprint", "config manual device fingerprint failed");

  std::filesystem::remove_all(dir);
}

void test_app_service_status_and_manual_device() {
  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Service";
  config.port = 0;
  config.discovery_enabled = false;
  config.manual_devices.push_back({"192.168.1.30", 53317, true, "Configured", "configured-fingerprint"});

  localsend::AppServiceOptions options;
  options.platform = localsend::PlatformKind::Desktop;
  options.enable_tls = false;
  options.device_model = "Service test";

  localsend::AppService service(config, options);
  auto status = service.status();
  require(!status.server_running, "service should start stopped");
  require(!status.https, "service should use HTTP when TLS disabled");
  require(status.alias == "Service", "service alias failed");
  require(service.self_info().device_model == "Service test", "service model failed");
  require(!service.announce_once(), "disabled discovery should not announce");
  require(service.refresh_discovery(std::chrono::milliseconds(1)) == 0, "disabled discovery should not refresh");
  require(!service.start_discovery(std::chrono::milliseconds(10), std::chrono::milliseconds(1)), "disabled discovery loop should not start");
  require(!service.status().discovery_running, "disabled discovery loop should stay stopped");
  require(status.device_count == 1, "service configured manual count failed");
  const auto configured = service.devices().get("fingerprint:configured-fingerprint");
  require(configured.has_value(), "service configured manual device missing");
  require(configured->source == localsend::DeviceSource::Manual, "service configured manual source failed");
  require(configured->device.https, "service configured manual protocol failed");

  const std::string key = service.add_manual_device("127.0.0.1", 53317, false, "Manual", "");
  require(key == "endpoint:127.0.0.1:53317", "service manual key failed");
  status = service.status();
  require(status.device_count == 2, "service device count failed");
  const auto snapshot = service.snapshot();
  require(snapshot.status.device_count == 2, "service snapshot device count failed");
  require(snapshot.status.transfer_count == 0, "service snapshot transfer count failed");
  require(snapshot.self.alias == "Service", "service snapshot self alias failed");
  require(snapshot.devices.size() == 2, "service snapshot devices failed");
  require(snapshot.transfers.empty(), "service snapshot transfers should be empty");
  const auto entry = service.devices().get(key);
  require(entry.has_value(), "service manual device missing");
  require(entry->source == localsend::DeviceSource::Manual, "service manual source failed");
}

void test_app_service_discovery_loop_lifecycle() {
  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Discovery Service";
  config.port = 0;
  config.discovery_enabled = true;

  localsend::AppServiceOptions options;
  options.enable_tls = false;
  localsend::AppService service(config, options);

  require(service.start_discovery(std::chrono::milliseconds(20), std::chrono::milliseconds(1)), "discovery loop should start");
  require(service.discovery_running(), "discovery loop running flag failed");
  require(service.status().discovery_running, "discovery loop status failed");
  require(service.start_discovery(std::chrono::milliseconds(20), std::chrono::milliseconds(1)), "discovery loop second start should be idempotent");
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  service.stop_discovery();
  require(!service.discovery_running(), "discovery loop should stop");
  require(!service.status().discovery_running, "discovery loop stopped status failed");
}

void test_app_service_update_and_save_config() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-service-config-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Before";
  config.port = 0;
  config.config_path = dir / "config.json";
  config.inbox_path = dir / "inbox";
  config.discovery_enabled = true;

  localsend::AppServiceOptions options;
  options.enable_tls = false;
  localsend::AppService service(config, options);

  auto updated = config;
  updated.alias = "After";
  updated.port = 0;
  updated.discovery_enabled = false;
  updated.auto_accept = true;
  updated.manual_devices.push_back({"192.168.1.40", 53317, false, "Updated Manual", ""});
  require(service.update_config(updated), "service config update failed");
  require(service.config().alias == "After", "service config alias update failed");
  require(service.self_info().alias == "After", "service self alias update failed");
  require(service.self_info().port == 0, "service self port update failed");
  require(!service.config().discovery_enabled, "service discovery config update failed");
  require(service.status().device_count == 1, "service config manual devices should load on update");
  require(service.devices().get("endpoint:192.168.1.40:53317").has_value(), "service updated manual device missing");
  require(service.save_config(), "service config save failed");

  const auto loaded = localsend::load_config(localsend::PlatformKind::Desktop, updated.config_path);
  require(loaded.alias == "After", "service saved alias failed");
  require(loaded.port == 0, "service saved port failed");
  require(!loaded.discovery_enabled, "service saved discovery failed");
  require(loaded.auto_accept, "service saved auto accept failed");
  require(loaded.manual_devices.size() == 1, "service saved manual device count failed");

  require(service.start_server(), "service config test server failed to start");
  auto rejected = updated;
  rejected.alias = "Rejected";
  require(!service.update_config(rejected), "running service config update should be rejected");
  require(service.config().alias == "After", "rejected config should not apply");
  service.stop_server();

  std::filesystem::remove_all(dir);
}

void test_app_service_register_updates_devices() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-service-register-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Register Receiver";
  config.port = 0;
  config.inbox_path = dir / "inbox";
  config.discovery_enabled = true;

  localsend::AppServiceOptions options;
  options.enable_tls = false;
  localsend::AppService service(config, options);
  require(service.start_server(), "service register test server failed to start");

  localsend::InfoRegisterDto peer;
  peer.alias = "Register Peer";
  peer.version = localsend::kProtocolVersion;
  peer.port = 4567;
  peer.protocol = localsend::ProtocolType::Http;
  peer.fingerprint = "register-peer";
  peer.device_model = "MateBook";

  const auto response = localsend::http_post("127.0.0.1",
                                             service.status().port,
                                             localsend::kRouteRegister,
                                             localsend::to_json(peer).dump());
  require(response.status == 200, "service register request failed");
  const auto snapshot = service.snapshot();
  require(snapshot.devices.size() == 1, "service register should add device");
  require(snapshot.devices[0].device.alias == "Register Peer", "service register device alias failed");
  require(snapshot.devices[0].device.ip == "127.0.0.1", "service register device ip failed");
  require(snapshot.devices[0].device.port == 4567, "service register device port failed");
  require(snapshot.devices[0].device.fingerprint == "register-peer", "service register device fingerprint failed");

  service.stop_server();
  std::filesystem::remove_all(dir);
}

void test_app_service_send_to_manual_device() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-service-tests";
  const auto inbox = dir / "inbox";
  const auto source_dir = dir / "source";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(inbox);
  std::filesystem::create_directories(source_dir);

  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Service Receiver";
  config.port = 0;
  config.inbox_path = inbox;
  config.certificate_path = dir / "cert.pem";
  config.private_key_path = dir / "key.pem";
  config.discovery_enabled = false;

  localsend::AppServiceOptions options;
  options.platform = localsend::PlatformKind::Desktop;
  options.enable_tls = true;

  localsend::AppService service(config, options);
  require(service.start_server(), "service server failed to start");
  auto status = service.status();
  require(status.server_running, "service server status failed");
  require(status.https, "service should use HTTPS");
  require(status.fingerprint.size() == 64, "service fingerprint failed");
  require(status.port > 0, "service port failed");

  const auto source = source_dir / "service.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "service transfer";
  }

  const std::string key = service.add_manual_device("127.0.0.1", status.port, true, "Loopback", status.fingerprint);
  require(service.send_files_to_device(key, {source}), "service send failed");
  require(std::filesystem::exists(inbox / "service.txt"), "service uploaded file missing");

  const auto transfers = service.transfers().snapshot();
  require(transfers.size() == 2, "service should track send and receive transfers");
  int completed = 0;
  for (const auto& transfer : transfers) {
    if (transfer.status == localsend::TransferStatus::Completed) {
      ++completed;
    }
  }
  require(completed == 2, "service transfers should complete");
  const auto snapshot = service.snapshot();
  require(snapshot.status.server_running, "service snapshot server status failed");
  require(snapshot.status.device_count == 1, "service snapshot send device count failed");
  require(snapshot.status.transfer_count == 2, "service snapshot send transfer count failed");
  require(snapshot.devices.size() == 1, "service snapshot send devices failed");
  require(snapshot.transfers.size() == 2, "service snapshot send transfers failed");

  service.stop_server();
  require(!service.status().server_running, "service should stop server");
  std::filesystem::remove_all(dir);
}

void test_app_service_async_send_to_manual_device() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-service-async-tests";
  const auto inbox = dir / "inbox";
  const auto source_dir = dir / "source";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(inbox);
  std::filesystem::create_directories(source_dir);

  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.alias = "Async Service Receiver";
  config.port = 0;
  config.inbox_path = inbox;
  config.discovery_enabled = false;

  localsend::AppServiceOptions options;
  options.enable_tls = false;
  localsend::AppService service(config, options);
  require(service.start_server(), "async service server failed to start");

  const auto source = source_dir / "async.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "async transfer";
  }

  const std::string key = service.add_manual_device("127.0.0.1", service.status().port, false, "Loopback", "");
  require(service.start_send_to_device(key, {source}), "async send should start");
  require(service.status().send_running, "async send status should be running");
  require(!service.start_send_to_device(key, {source}), "parallel async send should be rejected");
  require(service.status().last_send_error == "send already running", "parallel async send error should be reported");
  service.wait_for_send_idle();
  require(!service.status().send_running, "async send status should stop");
  require(service.status().last_send_error.empty(), "successful async send should clear error");
  require(std::filesystem::exists(inbox / "async.txt"), "async uploaded file missing");

  const auto transfers = service.transfers().snapshot();
  require(transfers.size() == 2, "async send should track send and receive transfers");
  for (const auto& transfer : transfers) {
    require(transfer.status == localsend::TransferStatus::Completed, "async transfer should complete");
  }

  service.stop_server();
  std::filesystem::remove_all(dir);
}

void test_app_service_send_start_errors() {
  auto config = localsend::default_config(localsend::PlatformKind::Desktop);
  config.discovery_enabled = false;

  localsend::AppServiceOptions options;
  options.enable_tls = false;
  localsend::AppService service(config, options);

  require(!service.start_send_to_device("missing", {}), "missing send target should fail");
  require(service.status().last_send_error == "selected peer disappeared", "missing send target error failed");

  const std::string key = service.add_manual_device("127.0.0.1", 9, false, "Loopback", "");
  require(!service.start_send_to_device(key, {}), "empty send file list should fail");
  require(service.status().last_send_error == "no files selected", "empty send file error failed");
  require(!service.cancel_current_send(), "idle send cancel should fail");
  require(service.status().last_send_error == "no active send", "idle send cancel error failed");
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

void test_file_browser_listing() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-file-browser-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "Zoo");
  std::filesystem::create_directories(dir / "alpha");
  {
    std::ofstream out(dir / "beta.txt", std::ios::binary);
    out << "beta";
  }
  {
    std::ofstream out(dir / "中文.txt", std::ios::binary);
    out << "utf8";
  }

  const auto listing = localsend::list_directory(dir);
  require(listing.path.filename() == dir.filename(), "file browser path failed");
  require(listing.has_parent, "file browser parent failed");
  require(listing.entries.size() == 4, "file browser entry count failed");
  require(listing.entries[0].directory && listing.entries[0].name == "alpha", "file browser directory sort alpha failed");
  require(listing.entries[1].directory && listing.entries[1].name == "Zoo", "file browser directory sort zoo failed");
  require(!listing.entries[2].directory && listing.entries[2].name == "beta.txt", "file browser file sort failed");
  require(listing.entries[2].size == 4, "file browser file size failed");
  require(!listing.entries[3].directory && listing.entries[3].name == "中文.txt", "file browser UTF-8 name failed");

  const auto files = localsend::selectable_files(listing);
  require(files.size() == 2, "file browser selectable file count failed");
  require(files[0].filename() == "beta.txt", "file browser selectable first failed");
  require(files[1].filename() == "中文.txt", "file browser selectable UTF-8 failed");
  const auto first = localsend::first_selectable_file(dir);
  require(first.has_value(), "file browser first selectable missing");
  require(first->filename() == "beta.txt", "file browser first selectable failed");
  const auto second = localsend::selectable_file_at(dir, 1);
  require(second.has_value(), "file browser indexed selectable missing");
  require(second->filename() == "中文.txt", "file browser indexed selectable failed");
  const auto wrapped = localsend::selectable_file_at(dir, 2);
  require(wrapped.has_value(), "file browser wrapped selectable missing");
  require(wrapped->filename() == "beta.txt", "file browser wrapped selectable failed");
  require(localsend::format_file_size(1536) == "1.5 KiB", "file browser size format failed");
  const auto choice = localsend::format_file_choice(dir, 1);
  require(choice.find("中文.txt (2/2, 4 B)") != std::string::npos, "file browser choice format failed");

  std::filesystem::remove_all(dir);
}

void test_prepare_outbox_creates_sample_file() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-outbox-tests";
  std::filesystem::remove_all(dir);

  const auto status = localsend::prepare_outbox(dir);
  require(status.directory_ready, "outbox directory should be ready");
  require(status.sample_ready, "outbox sample should be ready");
  require(status.selectable_count == 1, "outbox selectable count failed");
  require(status.sample_file.filename() == "localsend-handheld-test.txt", "outbox sample name failed");

  const auto listing = localsend::list_directory(dir);
  const auto files = localsend::selectable_files(listing);
  require(files.size() == 1, "outbox listing selectable count failed");
  require(files[0].filename() == "localsend-handheld-test.txt", "outbox listing sample failed");
  const auto first = localsend::first_selectable_file(dir);
  require(first.has_value(), "outbox first selectable missing");
  require(first->filename() == "localsend-handheld-test.txt", "outbox first selectable failed");

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

void test_http_send_downgrades_misadvertised_https_target() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-downgrade-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.port = 0;
  self.protocol = localsend::ProtocolType::Http;
  self.fingerprint = "receiver-fingerprint";

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start for HTTP downgrade test");

  const auto source = dir / "downgrade.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "plain http target";
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = server.port();
  target.version = localsend::kProtocolVersion;
  target.https = true;
  target.fingerprint = "receiver-fingerprint";
  target.alias = "Receiver";

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  require(localsend::send_single_file_http(target, source, sender), "misadvertised HTTPS target should fall back to HTTP");
  require(std::filesystem::exists(dir / "downgrade (1).txt"), "downgraded HTTP upload missing");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_info_and_register_discovery_semantics() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-register-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.port = 0;
  self.protocol = localsend::ProtocolType::Http;
  self.fingerprint = "receiver-fingerprint";

  localsend::LocalSendServer server(self, dir);
  localsend::Device registered;
  server.set_register_callback([&registered](localsend::Device device) {
    registered = std::move(device);
  });
  require(server.start(0), "server failed to start for register test");

  const auto self_info = localsend::http_get("127.0.0.1",
                                             server.port(),
                                             std::string(localsend::kRouteInfoV1) + "?fingerprint=receiver-fingerprint");
  require(self_info.status == 412, "info should reject self fingerprint");

  localsend::InfoRegisterDto peer;
  peer.alias = "Peer";
  peer.version = localsend::kProtocolVersion;
  peer.port = 53317;
  peer.protocol = localsend::ProtocolType::Https;
  peer.fingerprint = "peer-fingerprint";
  peer.device_model = "Phone";
  peer.device_type = localsend::DeviceType::Mobile;
  peer.download = true;

  const auto registered_response = localsend::http_post("127.0.0.1",
                                                        server.port(),
                                                        localsend::kRouteRegister,
                                                        localsend::to_json(peer).dump());
  require(registered_response.status == 200, "register route failed");
  const auto response_info = localsend::info_from_json(localsend::Json::parse(registered_response.body));
  require(response_info.alias == "Receiver", "register response should return local info");
  require(registered.alias == "Peer", "register callback alias failed");
  require(registered.ip == "127.0.0.1", "register callback ip failed");
  require(registered.port == 53317, "register callback port failed");
  require(registered.https, "register callback protocol failed");
  require(registered.fingerprint == "peer-fingerprint", "register callback fingerprint failed");
  require(registered.device_type == localsend::DeviceType::Mobile, "register callback device type failed");
  require(registered.download, "register callback download flag failed");

  peer.fingerprint = "receiver-fingerprint";
  const auto self_register = localsend::http_post("127.0.0.1",
                                                  server.port(),
                                                  localsend::kRouteRegister,
                                                  localsend::to_json(peer).dump());
  require(self_register.status == 412, "register should reject self fingerprint");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_https_server_routes_and_upload() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-https-tests";
  const auto source_dir = dir / "source";
  const auto inbox_dir = dir / "inbox";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(source_dir);
  std::filesystem::create_directories(inbox_dir);

  const std::string fingerprint = localsend::certificate_fingerprint_from_pem(test_tls_certificate());

  localsend::InfoRegisterDto self;
  self.alias = "Secure Receiver";
  self.port = 0;
  self.protocol = localsend::ProtocolType::Https;
  self.fingerprint = fingerprint;

  localsend::LocalSendServer server(self, inbox_dir, {test_tls_certificate(), test_tls_private_key()});
  require(server.start(0), "HTTPS server failed to start");

  const auto info = localsend::https_get("127.0.0.1", server.port(), localsend::kRouteInfo, fingerprint);
  require(info.status == 200, "HTTPS info route failed");
  const auto info_json = localsend::Json::parse(info.body);
  const auto decoded_info = localsend::info_from_json(info_json);
  require(decoded_info.alias == "Secure Receiver", "HTTPS info body failed");
  require(info_json.at("protocol").as_string() == "https", "HTTPS info protocol failed");
  require(decoded_info.fingerprint == fingerprint, "HTTPS info fingerprint failed");

  const auto source = source_dir / "secure.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "secure localsend upload";
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.version = localsend::kProtocolVersion;
  target.port = server.port();
  target.https = true;
  target.fingerprint = fingerprint;

  localsend::InfoRegisterDto sender;
  sender.alias = "Secure Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Https;
  sender.fingerprint = fingerprint;

  require(localsend::send_single_file_http(target, source, sender), "single file HTTPS send failed");
  require(std::filesystem::exists(inbox_dir / "secure.txt"), "HTTPS uploaded file missing");
  require(std::filesystem::file_size(inbox_dir / "secure.txt") == std::filesystem::file_size(source), "HTTPS uploaded size mismatch");

  localsend::Device bad_target = target;
  bad_target.fingerprint = std::string(64, '0');
  require(!localsend::send_single_file_http(bad_target, source, sender), "HTTPS send should reject wrong fingerprint");

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

void test_http_transfer_store_updates() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-transfer-store-tests";
  const auto source_dir = dir / "source";
  const auto inbox_dir = dir / "inbox";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(source_dir);
  std::filesystem::create_directories(inbox_dir);

  localsend::TransferStore receive_transfers;
  localsend::TransferStore send_transfers;

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, inbox_dir, &receive_transfers);
  require(server.start(0), "server failed to start for transfer store test");

  const auto source = source_dir / "tracked.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "tracked transfer";
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = server.port();
  target.alias = "Receiver";
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  require(localsend::send_files_http(target, {source}, sender, &send_transfers), "tracked HTTP send failed");

  const auto sent = send_transfers.snapshot();
  require(sent.size() == 1, "send transfer count failed");
  require(sent[0].direction == localsend::TransferDirection::Send, "send transfer direction failed");
  require(sent[0].status == localsend::TransferStatus::Completed, "send transfer should complete");
  require(sent[0].bytes_transferred == sent[0].size, "send transfer byte count failed");
  require(sent[0].peer_alias == "Receiver", "send transfer peer alias failed");
  require(sent[0].peer_ip == "127.0.0.1", "send transfer peer ip failed");

  const auto received = receive_transfers.snapshot();
  require(received.size() == 1, "receive transfer count failed");
  require(received[0].direction == localsend::TransferDirection::Receive, "receive transfer direction failed");
  require(received[0].status == localsend::TransferStatus::Completed, "receive transfer should complete");
  require(received[0].bytes_transferred == received[0].size, "receive transfer byte count failed");
  require(received[0].peer_alias == "Sender", "receive transfer peer alias failed");

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

void test_http_incomplete_upload_removes_partial_file() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-http-incomplete-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  localsend::InfoRegisterDto self;
  self.alias = "Receiver";
  self.protocol = localsend::ProtocolType::Http;

  localsend::LocalSendServer server(self, dir);
  require(server.start(0), "server failed to start for incomplete upload test");

  localsend::PrepareUploadRequestDto request;
  request.info.alias = "Sender";
  request.info.port = 12345;
  request.info.protocol = localsend::ProtocolType::Http;

  localsend::FileDto file;
  file.id = "partial-file";
  file.file_name = "partial.txt";
  file.size = 10;
  file.file_type = "text/plain";
  request.files.emplace(file.id, file);

  const auto prepare = localsend::http_post("127.0.0.1", server.port(), localsend::kRoutePrepareUpload, localsend::to_json(request).dump());
  require(prepare.status == 200, "prepare upload incomplete test failed");
  const auto response = localsend::prepare_upload_response_from_json(localsend::Json::parse(prepare.body));
  const std::string token = response.files.at(file.id);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "incomplete upload socket failed");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(server.port()));
  require(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "incomplete upload connect failed");

  const std::string path = std::string(localsend::kRouteUpload) + "?sessionId=" + response.session_id + "&fileId=" + file.id + "&token=" + token;
  const std::string request_text =
      "POST " + path + " HTTP/1.1\r\n"
      "Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n"
      "Connection: close\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 10\r\n\r\n"
      "short";
  require(::send(fd, request_text.data(), request_text.size(), 0) == static_cast<ssize_t>(request_text.size()), "incomplete upload send failed");
  ::shutdown(fd, SHUT_WR);

  char discard[1024];
  while (::recv(fd, discard, sizeof(discard), 0) > 0) {
  }
  ::close(fd);

  require(!std::filesystem::exists(dir / "partial.txt"), "incomplete upload should remove partial file");

  server.stop();
  std::filesystem::remove_all(dir);
}

void test_http_send_accepts_prepare_no_content() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-send-204-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto source = dir / "skip.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "skip me";
  }

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(listen_fd >= 0, "204 listen socket failed");
  int enabled = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "204 bind failed");
  require(::listen(listen_fd, 1) == 0, "204 listen failed");
  socklen_t len = sizeof(addr);
  require(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "204 getsockname failed");
  const int port = ntohs(addr.sin_port);

  std::thread server([listen_fd]() {
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client >= 0) {
      char discard[4096];
      static_cast<void>(::recv(client, discard, sizeof(discard), 0));
      const char* response =
          "HTTP/1.1 204 No Content\r\n"
          "Content-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      static_cast<void>(::send(client, response, std::strlen(response), 0));
      ::close(client);
    }
    ::close(listen_fd);
  });

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = port;
  target.version = localsend::kProtocolVersion;
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  localsend::TransferStore transfers;
  require(localsend::send_files_http(target, {source}, sender, &transfers), "204 prepare should be accepted");
  server.join();
  const auto snapshot = transfers.snapshot();
  require(snapshot.size() == 1, "204 transfer count failed");
  require(snapshot[0].status == localsend::TransferStatus::Completed, "204 transfer should be terminal");

  std::filesystem::remove_all(dir);
}

void test_http_send_reports_prepare_failure() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-send-prepare-failure-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto source = dir / "failed.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "failed";
  }

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(listen_fd >= 0, "prepare failure listen socket failed");
  int enabled = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "prepare failure bind failed");
  require(::listen(listen_fd, 1) == 0, "prepare failure listen failed");
  socklen_t len = sizeof(addr);
  require(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "prepare failure getsockname failed");
  const int port = ntohs(addr.sin_port);

  std::thread server([listen_fd]() {
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client >= 0) {
      char discard[4096];
      static_cast<void>(::recv(client, discard, sizeof(discard), 0));
      const char* body = "receiver busy";
      std::ostringstream response;
      response << "HTTP/1.1 500 Internal Server Error\r\n"
               << "Content-Type: text/plain\r\n"
               << "Content-Length: " << std::strlen(body) << "\r\n"
               << "Connection: close\r\n\r\n"
               << body;
      const std::string text = response.str();
      static_cast<void>(::send(client, text.data(), text.size(), 0));
      ::close(client);
    }
    ::close(listen_fd);
  });

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = port;
  target.version = localsend::kProtocolVersion;
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  localsend::TransferStore transfers;
  const localsend::SendFilesResult result = localsend::send_files_http_detailed(target, {source}, sender, &transfers);
  server.join();
  require(!result.ok, "prepare failure should fail detailed send");
  require(result.error.find("status=500") != std::string::npos, "prepare failure status should be reported");
  require(result.error.find("receiver busy") != std::string::npos, "prepare failure body should be reported");
  const auto snapshot = transfers.snapshot();
  require(snapshot.size() == 1, "prepare failure transfer count failed");
  require(snapshot[0].status == localsend::TransferStatus::Failed, "prepare failure transfer status failed");
  require(snapshot[0].error == result.error, "prepare failure transfer error should match result");

  std::filesystem::remove_all(dir);
}

void test_http_send_cancelled_before_prepare() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-send-cancel-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto source = dir / "cancel-before.txt";
  {
    std::ofstream out(source, std::ios::binary);
    out << "cancel before prepare";
  }

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = 9;
  target.version = localsend::kProtocolVersion;
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  localsend::TransferStore transfers;
  localsend::SendFilesControl control;
  control.cancel_requested = true;
  const localsend::SendFilesResult result = localsend::send_files_http_detailed(target, {source}, sender, &transfers, &control);
  require(!result.ok, "cancelled send should fail");
  require(result.cancelled, "cancelled send result flag failed");
  require(result.error == "send cancelled", "cancelled send error failed");
  const auto snapshot = transfers.snapshot();
  require(snapshot.size() == 1, "cancelled send transfer count failed");
  require(snapshot[0].status == localsend::TransferStatus::Cancelled, "cancelled send transfer status failed");

  std::filesystem::remove_all(dir);
}

void test_http_send_treats_missing_tokens_as_skipped() {
  const auto dir = std::filesystem::temp_directory_path() / "localsend-handheld-send-skipped-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto first = dir / "accepted.txt";
  const auto second = dir / "skipped.txt";
  {
    std::ofstream out(first, std::ios::binary);
    out << "accepted";
  }
  {
    std::ofstream out(second, std::ios::binary);
    out << "skipped";
  }

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(listen_fd >= 0, "skipped listen socket failed");
  int enabled = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "skipped bind failed");
  require(::listen(listen_fd, 2) == 0, "skipped listen failed");
  socklen_t len = sizeof(addr);
  require(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "skipped getsockname failed");
  const int port = ntohs(addr.sin_port);

  std::thread server([listen_fd]() {
    int client = ::accept(listen_fd, nullptr, nullptr);
    if (client >= 0) {
      char discard[4096];
      static_cast<void>(::recv(client, discard, sizeof(discard), 0));
      const char* body = R"({"sessionId":"session","files":{"0":"token"}})";
      std::ostringstream response;
      response << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << std::strlen(body) << "\r\n"
               << "Connection: close\r\n\r\n"
               << body;
      const std::string text = response.str();
      static_cast<void>(::send(client, text.data(), text.size(), 0));
      ::close(client);
    }

    client = ::accept(listen_fd, nullptr, nullptr);
    if (client >= 0) {
      char discard[4096];
      static_cast<void>(::recv(client, discard, sizeof(discard), 0));
      const char* response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: 2\r\n"
          "Connection: close\r\n\r\n{}";
      static_cast<void>(::send(client, response, std::strlen(response), 0));
      ::close(client);
    }
    ::close(listen_fd);
  });

  localsend::Device target;
  target.ip = "127.0.0.1";
  target.port = port;
  target.version = localsend::kProtocolVersion;
  target.https = false;

  localsend::InfoRegisterDto sender;
  sender.alias = "Sender";
  sender.port = 12345;
  sender.protocol = localsend::ProtocolType::Http;

  localsend::TransferStore transfers;
  require(localsend::send_files_http(target, {first, second}, sender, &transfers), "missing token should be treated as skipped");
  server.join();
  const auto snapshot = transfers.snapshot();
  require(snapshot.size() == 2, "skipped transfer count failed");
  require(snapshot[0].status == localsend::TransferStatus::Completed, "accepted transfer should complete");
  require(snapshot[1].status == localsend::TransferStatus::Completed, "skipped transfer should be terminal");

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
    test_transfer_store_lifecycle();
    test_transfer_store_cancel_and_strings();
    test_device_store_upsert_and_sources();
    test_device_store_offline_remove_and_clear();
    test_status_format_summaries();
    test_device_selection_online_cycle();
    test_security_fingerprint();
    test_tls_identity_persistence();
    test_mbedtls_linked();
    test_tls_loopback();
    test_route_constants();
    test_default_config_paths();
    test_config_round_trip();
    test_app_service_status_and_manual_device();
    test_app_service_discovery_loop_lifecycle();
    test_app_service_update_and_save_config();
    test_app_service_register_updates_devices();
    test_app_service_send_to_manual_device();
    test_app_service_async_send_to_manual_device();
    test_app_service_send_start_errors();
    test_safe_filename();
    test_unique_destination();
    test_file_browser_listing();
    test_prepare_outbox_creates_sample_file();
    test_http_server_routes_and_upload();
    test_http_send_downgrades_misadvertised_https_target();
    test_http_info_and_register_discovery_semantics();
    test_https_server_routes_and_upload();
    test_http_send_multiple_files();
    test_http_transfer_store_updates();
    test_http_v1_legacy_routes();
    test_http_cancel_rejects_upload();
    test_http_send_to_v1_target();
    test_http_prepare_uses_file_id();
    test_http_server_chunked_upload();
    test_http_incomplete_upload_removes_partial_file();
    test_http_send_accepts_prepare_no_content();
    test_http_send_reports_prepare_failure();
    test_http_send_cancelled_before_prepare();
    test_http_send_treats_missing_tokens_as_skipped();
    test_http_client_chunked_response();
  } catch (const std::exception& e) {
    std::cerr << "test failed: " << e.what() << '\n';
    return 1;
  }

  std::cout << "core tests passed\n";
  return 0;
}
