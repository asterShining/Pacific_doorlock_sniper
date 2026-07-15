#pragma once

#include "config.hpp"
#include "frame_slot.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

#ifndef DOORLOCK_FAKE_ONLY
// Forward declarations for Galaxy SDK types
typedef void* GX_DEV_HANDLE;
typedef int GX_STATUS;
struct GX_FRAME_BUFFER;
typedef GX_FRAME_BUFFER* PGX_FRAME_BUFFER;
#endif

class Camera {
 public:
  Camera(const Config& cfg, FrameExchange& frame_out, std::atomic<bool>& running);
  ~Camera();

  void run();

 private:
  void run_fake();

#ifndef DOORLOCK_FAKE_ONLY
  void run_real();
  void init_library();
  void configure_galaxy_environment();
  void open_device();
  void configure_device();
  bool convert_frame_to_bgr(const GX_FRAME_BUFFER& frame, std::string* error);
  bool convert_mono8_to_bgr(const uint8_t* src, int w, int h, std::string* error);
  bool convert_raw8_color_to_bgr(const uint8_t* src, int w, int h, std::string* error);
  bool convert_raw16_to_bgr(const uint8_t* src, int w, int h, bool color, int valid_bit, std::string* error);
  bool convert_packed_to_bgr(const GX_FRAME_BUFFER& frame, bool color, bool ten_bit, std::string* error);
  void ensure_buffers(int w, int h);

  GX_DEV_HANDLE device_handle_ = nullptr;
  bool library_initialized_ = false;
  bool stream_on_ = false;
  bool is_color_camera_ = false;
  int64_t color_filter_ = 0;
  int image_width_ = 0;
  int image_height_ = 0;

  std::vector<uint8_t> bgr_buffer_;
  std::vector<uint8_t> raw8_buffer_;
  std::vector<uint8_t> raw16_buffer_;
#endif

  const Config& cfg_;
  FrameExchange& frame_out_;
  std::atomic<bool>& running_;
};
