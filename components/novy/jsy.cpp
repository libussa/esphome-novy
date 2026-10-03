#include "jsy.h"

namespace esphome::novy {

uint16_t modbus_crc(const uint8_t *data, size_t size) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

std::array<uint8_t, 8> jsy194_request() {
  std::array<uint8_t, 8> request{0x01, 0x03, 0x00, 0x48, 0x00, 0x0E, 0, 0};
  const uint16_t crc = modbus_crc(request.data(), 6);
  request[6] = crc & 0xFF;
  request[7] = crc >> 8;
  return request;
}

static uint32_t register32(const uint8_t *frame, unsigned address) {
  const uint8_t *data = frame + 3 + (address - 0x48) * 4;
  return uint32_t{data[0]} << 24 | uint32_t{data[1]} << 16 | uint32_t{data[2]} << 8 | data[3];
}

bool parse_jsy194(const uint8_t (&frame)[JSY194_RESPONSE_SIZE], float (&power)[2]) {
  if (frame[0] != 0x01 || frame[1] != 0x03 || frame[2] != JSY194_RESPONSE_SIZE - 5)
    return false;
  const uint16_t crc = modbus_crc(frame, JSY194_RESPONSE_SIZE - 2);
  if (frame[JSY194_RESPONSE_SIZE - 2] != (crc & 0xFF) || frame[JSY194_RESPONSE_SIZE - 1] != crc >> 8)
    return false;
  // Magnitudes only: the 0x004E sign bytes merely reflect CT orientation,
  // and the hood is a pure load.
  power[0] = register32(frame, 0x4A) / 10000.0f;
  power[1] = register32(frame, 0x52) / 10000.0f;
  return true;
}

}  // namespace esphome::novy
