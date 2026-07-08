#pragma once

namespace localsend {

inline constexpr const char* kProtocolVersion = "2.1";
inline constexpr const char* kDefaultMulticastGroup = "224.0.0.167";
inline constexpr int kDefaultPort = 53317;
inline constexpr int kTransferBufferSize = 64 * 1024;

inline constexpr const char* kRouteInfo = "/api/localsend/v2/info";
inline constexpr const char* kRouteRegister = "/api/localsend/v2/register";
inline constexpr const char* kRoutePrepareUpload = "/api/localsend/v2/prepare-upload";
inline constexpr const char* kRouteUpload = "/api/localsend/v2/upload";
inline constexpr const char* kRouteCancel = "/api/localsend/v2/cancel";

} // namespace localsend

