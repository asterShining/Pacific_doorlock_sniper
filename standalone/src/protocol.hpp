#pragma once

#include <cstddef>
#include <cstdint>

namespace protocol {

constexpr uint8_t kMagic0 = 0x44u;
constexpr uint8_t kMagic1 = 0x4Cu;
constexpr uint8_t kVersion = 0x01u;
constexpr size_t kChunkBytes = 300u;
constexpr size_t kHeaderBytes = 10u;
constexpr size_t kChunkPayloadBytes = 288u;
constexpr size_t kCrcBytes = 2u;
constexpr uint8_t kFlagStreamReset = 0x01u;

}  // namespace protocol
