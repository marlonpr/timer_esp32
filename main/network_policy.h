#pragma once

#include <cstdint>

namespace factory_timer {

// All arguments are IPv4 addresses in host byte order.
bool IsAcceptedFactoryNetwork(uint32_t ip, uint32_t netmask, uint32_t gateway);

} // namespace factory_timer
