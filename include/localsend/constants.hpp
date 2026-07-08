#pragma once

namespace localsend {

inline constexpr const char* kProtocolVersion = "2.1";
inline constexpr const char* kDefaultMulticastGroup = "224.0.0.167";
inline constexpr const char* kDefaultBroadcastAddress = "255.255.255.255";
inline constexpr int kDefaultPort = 53317;
inline constexpr int kTransferBufferSize = 64 * 1024;

inline constexpr const char* kRouteInfo = "/api/localsend/v2/info";
inline constexpr const char* kRouteRegister = "/api/localsend/v2/register";
inline constexpr const char* kRoutePrepareUpload = "/api/localsend/v2/prepare-upload";
inline constexpr const char* kRouteUpload = "/api/localsend/v2/upload";
inline constexpr const char* kRouteCancel = "/api/localsend/v2/cancel";

inline constexpr const char* kRouteInfoV1 = "/api/localsend/v1/info";
inline constexpr const char* kRouteRegisterV1 = "/api/localsend/v1/register";
inline constexpr const char* kRoutePrepareUploadV1 = "/api/localsend/v1/send-request";
inline constexpr const char* kRouteUploadV1 = "/api/localsend/v1/send";
inline constexpr const char* kRouteCancelV1 = "/api/localsend/v1/cancel";

} // namespace localsend
