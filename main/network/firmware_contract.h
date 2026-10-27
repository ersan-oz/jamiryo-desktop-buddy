#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Stored immediately after ESP-IDF's app descriptor. This rejects older factory
// images and updates built for a different partition/model layout before erase.
struct JamiryoFirmwareContract {
    char magic[16];
    uint32_t layout;
    uint32_t detect_bytes;
    uint32_t feature_bytes;
    uint8_t detect_sha256[32];
    uint8_t feature_sha256[32];
};
static_assert(sizeof(JamiryoFirmwareContract) == 92);
inline constexpr size_t firmware_contract_offset = 24 + 8 + 256;
inline constexpr size_t firmware_prefix_size = firmware_contract_offset + sizeof(JamiryoFirmwareContract);

inline bool jamiryo_firmware_compatible(const uint8_t *bytes, size_t size,
                                      const JamiryoFirmwareContract &expected)
{
    if (!bytes || size < firmware_prefix_size || bytes[0] != 0xe9
        || !bytes[1] || bytes[1] > 16 || bytes[12] != 9 || bytes[13] != 0) return false;
    uint32_t app_magic;
    std::memcpy(&app_magic, bytes + 32, sizeof(app_magic));
    const char project[32] = "desktop_buddy";
    return app_magic == 0xabcd5432 && !std::memcmp(bytes + 80, project, sizeof(project))
        && !std::memcmp(bytes + firmware_contract_offset, &expected, sizeof(expected));
}
