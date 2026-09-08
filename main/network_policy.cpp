#include "network_policy.h"

namespace factory_timer {

namespace {
constexpr uint32_t kExpectedNetwork = 0xC0A80000u;  // 192.168.0.0
constexpr uint32_t kExpectedNetmask = 0xFFFF0000u;  // 255.255.0.0
constexpr uint32_t kExpectedGateway = 0xC0A80001u;  // 192.168.0.1
} // namespace

bool IsAcceptedFactoryNetwork(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    return netmask == kExpectedNetmask &&
           (ip & netmask) == kExpectedNetwork &&
           gateway == kExpectedGateway;
}

} // namespace factory_timer
