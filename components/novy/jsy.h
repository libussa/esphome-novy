#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::novy {

// JSY-MK-194G/194T Modbus RTU read of its 14 four-byte registers 0x0048-0x0055.
// Register map and scaling follow the JSY-MK-194G/194T manuals and MycilaJSY
// (MIT, Mathieu Carbou): https://github.com/mathieucarbou/MycilaJSY
// Response: address, function, byte count, 56 data bytes, CRC (low first).
constexpr size_t JSY194_RESPONSE_SIZE = 61;

uint16_t modbus_crc(const uint8_t *data, size_t size);
std::array<uint8_t, 8> jsy194_request();
// Checks address 1, function 3, byte count and CRC; returns CH1/CH2 watts.
bool parse_jsy194(const uint8_t (&frame)[JSY194_RESPONSE_SIZE], float (&power)[2]);

}  // namespace esphome::novy
