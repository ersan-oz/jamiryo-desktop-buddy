#pragma once
#include <cstdint>

// Isolated setup AP only: no routing/NAT to the home network or Internet.
// Android's private-IP DNS check skips HTTP probes for RFC1918 addresses.
// Use the reserved benchmark range for this local compatibility network.
#define BUDDY_SETUP_IP "198.18.0.1"
#define BUDDY_SETUP_ORIGIN "http://" BUDDY_SETUP_IP
#define BUDDY_SETUP_URL BUDDY_SETUP_ORIGIN "/"
namespace buddy_setup {
inline constexpr uint32_t subnet = 0xc6120000U;
inline constexpr uint8_t address[] = {198, 18, 0, 1};
}
