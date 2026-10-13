#include "network_policy.h"
#include <charconv>
#include <algorithm>

namespace factory_timer {

namespace {
constexpr uint32_t kExpectedNetwork = 0xC0A80000u;  // 192.168.0.0
constexpr uint32_t kExpectedNetmask = 0xFFFFFF00u;  // 255.255.255.0  /24
constexpr uint32_t kExpectedGateway = 0xC0A80001u;  // 192.168.0.1
} // namespace

bool ParseBssid(std::string_view text, std::array<uint8_t, 6>& value) {
    if (text.size() != 17) return false;
    std::array<uint8_t, 6> parsed{};
    for (size_t i = 0; i < parsed.size(); ++i) {
        const size_t offset = i * 3;
        if (i < 5 && text[offset + 2] != ':') return false;
        unsigned byte{};
        const auto result = std::from_chars(text.data() + offset, text.data() + offset + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + offset + 2 || byte > 255) return false;
        parsed[i] = static_cast<uint8_t>(byte);
    }
    if ((parsed[0] & 1) || std::all_of(parsed.begin(), parsed.end(), [](uint8_t x){return x == 0;})) return false;
    value = parsed;
    return true;
}

bool IsAcceptedFactoryNetwork(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    return netmask == kExpectedNetmask &&
           (ip & netmask) == kExpectedNetwork &&
           gateway == kExpectedGateway;
}

} // namespace factory_timer
