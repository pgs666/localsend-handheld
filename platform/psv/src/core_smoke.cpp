#include "localsend/app_service.hpp"
#include "localsend/config.hpp"
#include "localsend/discovery.hpp"
#include "localsend/http.hpp"
#include "localsend/protocol.hpp"
#include "localsend/security.hpp"
#include "localsend/tls.hpp"

#include <cstdlib>
#include <string>

int main()
{
    const localsend::AppConfig config = localsend::default_config(localsend::PlatformKind::Psv);
    localsend::InfoRegisterDto info;
    info.alias = config.alias;
    info.port = config.port;
    info.protocol = localsend::ProtocolType::Http;
    info.device_model = "PlayStation Vita";
    info.device_type = localsend::DeviceType::Mobile;

    const std::string encoded = localsend::to_json(info).dump();
    const localsend::InfoRegisterDto decoded = localsend::info_register_from_json(localsend::Json::parse(encoded));
    return decoded.alias == config.alias && decoded.port == config.port ? EXIT_SUCCESS : EXIT_FAILURE;
}
