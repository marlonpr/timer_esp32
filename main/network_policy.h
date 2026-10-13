#pragma once

#include <cstdint>
#include <array>
#include <string_view>

namespace factory_timer {

// All arguments are IPv4 addresses in host byte order.
bool ParseBssid(std::string_view text, std::array<uint8_t, 6>& value);

bool IsAcceptedFactoryNetwork(uint32_t ip, uint32_t netmask, uint32_t gateway);

} // namespace factory_timer
