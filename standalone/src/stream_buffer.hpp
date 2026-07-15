#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

template <size_t Capacity>
class CircularStreamBuffer {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

 public:
  size_t size() const { return tail_ - head_; }
  size_t available() const { return Capacity - size(); }
  bool empty() const { return head_ == tail_; }

  void append(const uint8_t* src, size_t len) {
    if (len > available()) {
      // Drop oldest data to make room
      head_ += len - available();
    }
    for (size_t i = 0; i < len; ++i) {
      data_[(tail_ + i) & kMask] = src[i];
    }
    tail_ += len;
  }

  // Copy 'len' bytes starting from logical offset 0 (head) into dst
  void peek(uint8_t* dst, size_t offset, size_t len) const {
    for (size_t i = 0; i < len; ++i) {
      dst[i] = data_[(head_ + offset + i) & kMask];
    }
  }

  // Discard the first 'len' bytes
  void consume(size_t len) {
    if (len > size()) len = size();
    head_ += len;
  }

  // Random access relative to head
  uint8_t operator[](size_t offset) const {
    return data_[(head_ + offset) & kMask];
  }

  // Drop 'len' bytes from the front
  void drop_front(size_t len) { consume(len); }

  void clear() {
    head_ = 0;
    tail_ = 0;
  }

 private:
  static constexpr size_t kMask = Capacity - 1;
  std::array<uint8_t, Capacity> data_{};
  size_t head_ = 0;
  size_t tail_ = 0;
};
