#include "localsend/constants.hpp"
#include "localsend/config.hpp"
#include "localsend/discovery.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"
#include "localsend/safe_path.hpp"
#include "localsend/security.hpp"
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
}

void test_default_config_paths() {
  const auto switch_config = localsend::default_config(localsend::PlatformKind::Switch);
  require(switch_config.inbox_path.string() == "sdmc:/switch/localsend/inbox/", "switch inbox path mismatch");
  require(switch_config.config_path.string() == "sdmc:/switch/localsend/config.json", "switch config path mismatch");
  require(switch_config.certificate_path.string() == "sdmc:/switch/localsend/cert.pem", "switch certificate path mismatch");
  require(switch_config.private_key_path.string() == "sdmc:/switch/localsend/key.pem", "switch private key path mismatch");

  const auto psv_config = localsend::default_config(localsend::PlatformKind::Psv);
  require(psv_config.inbox_path.string() == "ux0:data/localsend/inbox/", "psv inbox path mismatch");
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
  config.certificate_path = "tls/cert.pem";
  config.private_key_path = "tls/key.pem";
  config.port = 12345;
  config.discovery_enabled = false;
  config.auto_accept = true;
  localsend::save_config(config, path);

  const auto loaded = localsend::load_config(localsend::PlatformKind::Desktop, path);
  require(loaded.alias == "Desk", "config alias round trip failed");
  require(loaded.inbox_path.string() == "downloads", "config inbox round trip failed");
  require(loaded.certificate_path.string() == "tls/cert.pem", "config certificate round trip failed");
  require(loaded.private_key_path.string() == "tls/key.pem", "config private key round trip failed");
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
    test_security_fingerprint();
    test_tls_identity_persistence();
    test_mbedtls_linked();
    test_tls_loopback();
    test_route_constants();
    test_default_config_paths();
    test_config_round_trip();
    test_safe_filename();
    test_unique_destination();
    test_http_server_routes_and_upload();
    test_https_server_routes_and_upload();
    test_http_send_multiple_files();
    test_http_v1_legacy_routes();
    test_http_cancel_rejects_upload();
    test_http_send_to_v1_target();
    test_http_prepare_uses_file_id();
    test_http_server_chunked_upload();
    test_http_incomplete_upload_removes_partial_file();
    test_http_client_chunked_response();
  } catch (const std::exception& e) {
    std::cerr << "test failed: " << e.what() << '\n';
    return 1;
  }

  std::cout << "core tests passed\n";
  return 0;
}
