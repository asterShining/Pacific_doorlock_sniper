#pragma once

#include "config.hpp"
#include "frame_slot.hpp"

#include <atomic>

class Display {
 public:
  Display(const Config& cfg, DisplayExchange& display_in, std::atomic<bool>& running);

  void run();  // Main loop, call from dedicated thread

 private:
  const Config& cfg_;
  DisplayExchange& display_in_;
  std::atomic<bool>& running_;
  uint64_t frame_counter_ = 0;
};
