#pragma once
#include <stdint.h>
// zlib-compatible CRC-32, as esp_rom_crc32_le(0, ...) on the chip
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
