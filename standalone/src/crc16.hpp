#pragma once

#include <cstddef>
#include <cstdint>

inline uint16_t crc16_ccitt(const uint8_t* data, size_t size) {
  uint16_t crc = 0xFFFFu;
  if (data == nullptr) return crc;

  for (size_t idx = 0u; idx < size; ++idx) {
    crc ^= data[idx];
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 0x0001u) != 0u) {
        crc = static_cast<uint16_t>((crc >> 1) ^ 0x8408u);
      } else {
        crc = static_cast<uint16_t>(crc >> 1);
      }
    }
  }
  return crc;
}
