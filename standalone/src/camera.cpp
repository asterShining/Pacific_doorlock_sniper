#include "camera.hpp"
#include "log.hpp"

#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#ifndef DOORLOCK_FAKE_ONLY
#include "DxImageProc.h"
#include "GxIAPI.h"
#include "GxPixelFormat.h"
#include <dlfcn.h>

namespace {
constexpr uint64_t kAcquisitionBufferNum = 5;
constexpr int64_t kUsbTransferSize = 64 * 1024;
constexpr int64_t kUsbTransferUrbNum = 64;

bool is_node_writable(GX_NODE_ACCESS_MODE mode) {
  return mode == GX_NODE_ACCESS_MODE_WO || mode == GX_NODE_ACCESS_MODE_RW;
}

void set_optional_int_node(GX_DEV_HANDLE dev, const char* name, int64_t value) {
  GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
  if (GXGetNodeAccessMode(dev, name, &mode) != GX_STATUS_SUCCESS || !is_node_writable(mode))
    return;
  GXSetIntValue(dev, name, value);
}

void set_optional_float_node(GX_DEV_HANDLE dev, const char* name, double value) {
  GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
  if (GXGetNodeAccessMode(dev, name, &mode) != GX_STATUS_SUCCESS || !is_node_writable(mode))
    return;
  GXSetFloatValue(dev, name, value);
}

void set_optional_bool_node(GX_DEV_HANDLE dev, const char* name, bool value) {
  GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
  if (GXGetNodeAccessMode(dev, name, &mode) != GX_STATUS_SUCCESS || !is_node_writable(mode))
    return;
  GXSetBoolValue(dev, name, value);
}

void set_optional_enum_node(GX_DEV_HANDLE dev, const char* name, const char* value) {
  GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
  if (GXGetNodeAccessMode(dev, name, &mode) != GX_STATUS_SUCCESS || !is_node_writable(mode))
    return;
  GXSetEnumValueByString(dev, name, value);
}

std::string last_error_string(GX_STATUS status) {
  GX_STATUS queried = status;
  size_t size = 0;
  if (GXGetLastError(&queried, nullptr, &size) != GX_STATUS_SUCCESS || size == 0)
    return "No SDK error message";
  std::string msg(size, '\0');
  if (GXGetLastError(&queried, msg.data(), &size) != GX_STATUS_SUCCESS)
    return "Failed to query error";
  if (!msg.empty() && msg.back() == '\0') msg.pop_back();
  return msg;
}
}  // namespace
#endif  // DOORLOCK_FAKE_ONLY

// ========== Constructor / Destructor ==========

Camera::Camera(const Config& cfg, FrameExchange& frame_out, std::atomic<bool>& running)
    : cfg_(cfg), frame_out_(frame_out), running_(running) {}

Camera::~Camera() {
#ifndef DOORLOCK_FAKE_ONLY
  if (stream_on_ && device_handle_) {
    GXStreamOff(device_handle_);
    stream_on_ = false;
  }
  if (device_handle_) {
    GXCloseDevice(device_handle_);
    device_handle_ = nullptr;
  }
  if (library_initialized_) {
    GXCloseLib();
    library_initialized_ = false;
  }
#endif
}

// ========== run() — dispatches to fake or real ==========

void Camera::run() {
  if (cfg_.camera.fake) {
    run_fake();
    return;
  }
#ifdef DOORLOCK_FAKE_ONLY
  LOG_FATAL("Real camera requested but built with FAKE_ONLY. Use --set camera.fake=true");
#else
  run_real();
#endif
}

// ========== Fake camera ==========

void Camera::run_fake() {
  LOG_INFO("Fake camera: generating synthetic %dx%d frames at %d fps",
           1440, 1080, cfg_.encoder.output_fps);

  int seq = 0;
  const int64_t interval_us = 1000000 / std::max(cfg_.encoder.output_fps, 1);

  while (running_.load(std::memory_order_acquire)) {
    cv::Mat frame(1080, 1440, CV_8UC3, cv::Scalar(30, 30, 30));

    // Moving white rectangle (simulates a target)
    int bx = (seq * 7) % 1360;
    int by = 400 + static_cast<int>(120.0 * std::sin(seq * 0.05));
    cv::rectangle(frame, cv::Rect(bx, by, 80, 80), cv::Scalar(255, 255, 255), cv::FILLED);

    // Small moving dot (second target)
    int dx = 720 + static_cast<int>(300.0 * std::cos(seq * 0.03));
    int dy = 540 + static_cast<int>(200.0 * std::sin(seq * 0.04));
    cv::circle(frame, cv::Point(dx, dy), 15, cv::Scalar(0, 200, 255), cv::FILLED);

    // Frame number text
    cv::putText(frame, "FAKE #" + std::to_string(seq),
                cv::Point(50, 80), cv::FONT_HERSHEY_SIMPLEX, 1.5,
                cv::Scalar(0, 255, 0), 2);

    // Grid lines
    for (int gx = 0; gx < 1440; gx += 360)
      cv::line(frame, cv::Point(gx, 0), cv::Point(gx, 1079), cv::Scalar(50, 50, 50));
    for (int gy = 0; gy < 1080; gy += 270)
      cv::line(frame, cv::Point(0, gy), cv::Point(1439, gy), cv::Scalar(50, 50, 50));

    // Center crosshair
    cv::line(frame, cv::Point(710, 530), cv::Point(730, 550), cv::Scalar(0, 0, 200), 2);
    cv::line(frame, cv::Point(730, 530), cv::Point(710, 550), cv::Scalar(0, 0, 200), 2);

    frame_out_.write(frame);
    seq++;
    std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
  }

  LOG_INFO("Fake camera stopped after %d frames", seq);
}

// ========== Real camera (Galaxy SDK) ==========

#ifndef DOORLOCK_FAKE_ONLY

void Camera::run_real() {
  init_library();
  open_device();
  configure_device();

  GXSetFloatValue(device_handle_, "ExposureTime", cfg_.camera.exposure_time);
  GXSetFloatValue(device_handle_, "Gain", cfg_.camera.gain);

  GX_STATUS status = GXStreamOn(device_handle_);
  if (status != GX_STATUS_SUCCESS) {
    LOG_FATAL("GXStreamOn failed: [%d] %s", status, last_error_string(status).c_str());
    return;
  }
  stream_on_ = true;
  LOG_INFO("Camera capture started");

  int fail_count = 0;
  while (running_.load(std::memory_order_acquire)) {
    PGX_FRAME_BUFFER frame = nullptr;
    status = GXDQBuf(device_handle_, &frame, 1000);

    if (!running_.load(std::memory_order_acquire)) break;
    if (status == GX_STATUS_TIMEOUT) continue;

    if (status != GX_STATUS_SUCCESS) {
      LOG_WARN("GXDQBuf failed: [%d] %s", status, last_error_string(status).c_str());
      if (++fail_count > 5) {
        LOG_FATAL("Camera acquisition failed repeatedly, attempting reconnect...");
        GXStreamOff(device_handle_);
        stream_on_ = false;
        GXCloseDevice(device_handle_);
        device_handle_ = nullptr;
        fail_count = 0;
        while (running_.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          try {
            open_device();
            configure_device();
            GXSetFloatValue(device_handle_, "ExposureTime", cfg_.camera.exposure_time);
            GXSetFloatValue(device_handle_, "Gain", cfg_.camera.gain);
            status = GXStreamOn(device_handle_);
            if (status == GX_STATUS_SUCCESS) {
              stream_on_ = true;
              LOG_INFO("Camera reconnected successfully");
              break;
            }
          } catch (...) {
            LOG_WARN("Camera reconnect attempt failed, retrying...");
          }
        }
      }
      continue;
    }

    if (frame == nullptr) continue;

    bool frame_ok = (frame->nStatus == GX_FRAME_STATUS_SUCCESS);
    if (!frame_ok) {
      LOG_WARN("Abnormal frame status: %d", frame->nStatus);
    } else {
      ensure_buffers(frame->nWidth, frame->nHeight);
      std::string error;
      frame_ok = convert_frame_to_bgr(*frame, &error);
      if (!frame_ok) LOG_ERROR("Frame conversion failed: %s", error.c_str());
    }

    GXQBuf(device_handle_, frame);

    if (frame_ok) {
      cv::Mat bgr(image_height_, image_width_, CV_8UC3, bgr_buffer_.data());
      frame_out_.write(bgr);
      fail_count = 0;
    }
  }

  if (stream_on_) {
    GXStreamOff(device_handle_);
    stream_on_ = false;
  }
  LOG_INFO("Camera capture stopped");
}

void Camera::init_library() {
  configure_galaxy_environment();
  GX_STATUS status = GXInitLib();
  if (status != GX_STATUS_SUCCESS) {
    LOG_FATAL("GXInitLib failed: [%d] %s", status, last_error_string(status).c_str());
    throw std::runtime_error("GXInitLib failed");
  }
  library_initialized_ = true;
}

void Camera::configure_galaxy_environment() {
  if (std::getenv("GENICAM_GENTL64_PATH") != nullptr) return;
  Dl_info lib_info{};
  if (dladdr(reinterpret_cast<void*>(&GXInitLib), &lib_info) == 0 || lib_info.dli_fname == nullptr) {
    LOG_WARN("Failed to resolve libgxiapi path");
    return;
  }
  auto gentl_dir = std::filesystem::path(lib_info.dli_fname).parent_path();
  if (!std::filesystem::exists(gentl_dir / "GxU3VTL.cti")) {
    LOG_WARN("GxU3VTL.cti not found beside libgxiapi.so: %s", gentl_dir.string().c_str());
    return;
  }
  setenv("GENICAM_GENTL64_PATH", gentl_dir.c_str(), 1);
  LOG_INFO("Set GENICAM_GENTL64_PATH=%s", gentl_dir.string().c_str());
}

void Camera::open_device() {
  while (running_.load(std::memory_order_acquire)) {
    uint32_t device_count = 0;
    GX_STATUS status = GXUpdateAllDeviceList(&device_count, 1000);
    if (status != GX_STATUS_SUCCESS) {
      LOG_WARN("Enumerate cameras failed: [%d] %s", status, last_error_string(status).c_str());
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    if (device_count == 0) {
      LOG_WARN("No Daheng camera found, retrying...");
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    LOG_INFO("Found %u Daheng camera(s)", device_count);
    status = GXOpenDeviceByIndex(1, &device_handle_);
    if (status != GX_STATUS_SUCCESS) {
      LOG_WARN("Open camera failed: [%d] %s, retrying...",
               status, last_error_string(status).c_str());
      device_handle_ = nullptr;
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    GX_STRING_VALUE vendor{}, model{}, serial{};
    if (GXGetStringValue(device_handle_, "DeviceVendorName", &vendor) == GX_STATUS_SUCCESS)
      LOG_INFO("Vendor: %s", vendor.strCurValue);
    if (GXGetStringValue(device_handle_, "DeviceModelName", &model) == GX_STATUS_SUCCESS)
      LOG_INFO("Model: %s", model.strCurValue);
    if (GXGetStringValue(device_handle_, "DeviceSerialNumber", &serial) == GX_STATUS_SUCCESS)
      LOG_INFO("Serial: %s", serial.strCurValue);

    GX_NODE_ACCESS_MODE access = GX_NODE_ACCESS_MODE_NI;
    status = GXGetNodeAccessMode(device_handle_, "PixelColorFilter", &access);
    if (status == GX_STATUS_SUCCESS && (access == GX_NODE_ACCESS_MODE_RO ||
        access == GX_NODE_ACCESS_MODE_WO || access == GX_NODE_ACCESS_MODE_RW)) {
      GX_ENUM_VALUE pixel_filter{};
      GXGetEnumValue(device_handle_, "PixelColorFilter", &pixel_filter);
      color_filter_ = pixel_filter.stCurValue.nCurValue;
      is_color_camera_ = (color_filter_ != GX_COLOR_FILTER_NONE);
    } else {
      is_color_camera_ = false;
      color_filter_ = GX_COLOR_FILTER_NONE;
    }
    LOG_INFO("Camera type: %s", is_color_camera_ ? "color" : "monochrome");

    GX_INT_VALUE wv{}, hv{};
    GXGetIntValue(device_handle_, "Width", &wv);
    GXGetIntValue(device_handle_, "Height", &hv);
    image_width_ = static_cast<int>(wv.nCurValue);
    image_height_ = static_cast<int>(hv.nCurValue);
    LOG_INFO("Image size: %d x %d", image_width_, image_height_);
    return;
  }
  throw std::runtime_error("Shutdown while waiting for camera");
}

void Camera::configure_device() {
  GXSetEnumValueByString(device_handle_, "AcquisitionMode", "Continuous");
  GXSetEnumValueByString(device_handle_, "TriggerMode", "Off");
  GXSetAcqusitionBufferNumber(device_handle_, kAcquisitionBufferNum);
  set_optional_int_node(device_handle_, "StreamTransferSize", kUsbTransferSize);
  set_optional_int_node(device_handle_, "StreamTransferNumberUrb", kUsbTransferUrbNum);
  if (is_color_camera_) {
    set_optional_enum_node(device_handle_, "BalanceWhiteAuto", "Once");
    // 优先把颜色观感拉回“自然可看”，目的是当前链路首先要避免灯条和高光区域过饱和发炸，
    // 再通过较温和的 Gamma 把偏暗画面提起来；这些都在相机侧完成，能减少后续编码前就已经失真的风险。
    set_optional_enum_node(device_handle_, "SaturationMode", "On");
    set_optional_int_node(device_handle_, "Saturation", cfg_.camera.saturation);
    set_optional_float_node(device_handle_, "Contrast", cfg_.camera.contrast);
  }
  set_optional_bool_node(device_handle_, "GammaEnable", true);
  set_optional_enum_node(device_handle_, "GammaMode", "User");
  set_optional_float_node(device_handle_, "Gamma", cfg_.camera.gamma);

  LOG_INFO("Camera tuning: exposure=%.0fus gain=%.2f gamma=%.2f saturation=%d contrast=%.2f",
           cfg_.camera.exposure_time,
           cfg_.camera.gain,
           cfg_.camera.gamma,
           cfg_.camera.saturation,
           cfg_.camera.contrast);
}

void Camera::ensure_buffers(int w, int h) {
  image_width_ = w;
  image_height_ = h;
  size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (bgr_buffer_.size() != pixels * 3) bgr_buffer_.resize(pixels * 3);
  if (raw8_buffer_.size() != pixels) raw8_buffer_.resize(pixels);
  if (raw16_buffer_.size() != pixels * 2) raw16_buffer_.resize(pixels * 2);
}

bool Camera::convert_frame_to_bgr(const GX_FRAME_BUFFER& frame, std::string* error) {
  const auto* buf = static_cast<const uint8_t*>(frame.pImgBuf);
  int w = frame.nWidth, h = frame.nHeight;
  switch (frame.nPixelFormat) {
    case GX_PIXEL_FORMAT_MONO8:
    case GX_PIXEL_FORMAT_MONO8_SIGNED:
      return convert_mono8_to_bgr(buf, w, h, error);
    case GX_PIXEL_FORMAT_BAYER_GR8: case GX_PIXEL_FORMAT_BAYER_RG8:
    case GX_PIXEL_FORMAT_BAYER_GB8: case GX_PIXEL_FORMAT_BAYER_BG8:
      return convert_raw8_color_to_bgr(buf, w, h, error);
    case GX_PIXEL_FORMAT_MONO10:
      return convert_raw16_to_bgr(buf, w, h, false, DX_BIT_2_9, error);
    case GX_PIXEL_FORMAT_BAYER_GR10: case GX_PIXEL_FORMAT_BAYER_RG10:
    case GX_PIXEL_FORMAT_BAYER_GB10: case GX_PIXEL_FORMAT_BAYER_BG10:
      return convert_raw16_to_bgr(buf, w, h, true, DX_BIT_2_9, error);
    case GX_PIXEL_FORMAT_MONO12:
      return convert_raw16_to_bgr(buf, w, h, false, DX_BIT_4_11, error);
    case GX_PIXEL_FORMAT_BAYER_GR12: case GX_PIXEL_FORMAT_BAYER_RG12:
    case GX_PIXEL_FORMAT_BAYER_GB12: case GX_PIXEL_FORMAT_BAYER_BG12:
      return convert_raw16_to_bgr(buf, w, h, true, DX_BIT_4_11, error);
    case GX_PIXEL_FORMAT_MONO10_P: case GX_PIXEL_FORMAT_MONO10_PACKED:
      return convert_packed_to_bgr(frame, false, true, error);
    case GX_PIXEL_FORMAT_MONO12_P: case GX_PIXEL_FORMAT_MONO12_PACKED:
      return convert_packed_to_bgr(frame, false, false, error);
    case GX_PIXEL_FORMAT_BAYER_GR10_P: case GX_PIXEL_FORMAT_BAYER_RG10_P:
    case GX_PIXEL_FORMAT_BAYER_GB10_P: case GX_PIXEL_FORMAT_BAYER_BG10_P:
    case GX_PIXEL_FORMAT_BAYER_GR10_PACKED: case GX_PIXEL_FORMAT_BAYER_RG10_PACKED:
    case GX_PIXEL_FORMAT_BAYER_GB10_PACKED: case GX_PIXEL_FORMAT_BAYER_BG10_PACKED:
      return convert_packed_to_bgr(frame, true, true, error);
    case GX_PIXEL_FORMAT_BAYER_GR12_P: case GX_PIXEL_FORMAT_BAYER_RG12_P:
    case GX_PIXEL_FORMAT_BAYER_GB12_P: case GX_PIXEL_FORMAT_BAYER_BG12_P:
    case GX_PIXEL_FORMAT_BAYER_GR12_PACKED: case GX_PIXEL_FORMAT_BAYER_RG12_PACKED:
    case GX_PIXEL_FORMAT_BAYER_GB12_PACKED: case GX_PIXEL_FORMAT_BAYER_BG12_PACKED:
      return convert_packed_to_bgr(frame, true, false, error);
    default:
      *error = "Unsupported pixel format: " + std::to_string(frame.nPixelFormat);
      return false;
  }
}

bool Camera::convert_mono8_to_bgr(const uint8_t* src, int w, int h, std::string* error) {
  cv::Mat mono(h, w, CV_8UC1, const_cast<uint8_t*>(src));
  cv::Mat bgr(h, w, CV_8UC3, bgr_buffer_.data());
  cv::cvtColor(mono, bgr, cv::COLOR_GRAY2BGR);
  if (bgr.empty()) { *error = "cvtColor returned empty"; return false; }
  return true;
}

bool Camera::convert_raw8_color_to_bgr(const uint8_t* src, int w, int h, std::string* error) {
  VxInt32 dx = DxRaw8toRGB24Ex(
      const_cast<uint8_t*>(src), bgr_buffer_.data(),
      static_cast<VxUint32>(w), static_cast<VxUint32>(h),
      RAW2RGB_NEIGHBOUR, static_cast<DX_PIXEL_COLOR_FILTER>(color_filter_), false,
      DX_ORDER_BGR);
  if (dx != DX_OK) { *error = "DxRaw8toRGB24Ex failed: " + std::to_string(dx); return false; }
  return true;
}

bool Camera::convert_raw16_to_bgr(const uint8_t* src, int w, int h, bool color, int valid_bit, std::string* error) {
  VxInt32 dx = DxRaw16toRaw8(
      const_cast<uint8_t*>(src), raw8_buffer_.data(),
      static_cast<VxUint32>(w), static_cast<VxUint32>(h),
      static_cast<DX_VALID_BIT>(valid_bit));
  if (dx != DX_OK) { *error = "DxRaw16toRaw8 failed: " + std::to_string(dx); return false; }
  if (color) return convert_raw8_color_to_bgr(raw8_buffer_.data(), w, h, error);
  return convert_mono8_to_bgr(raw8_buffer_.data(), w, h, error);
}

bool Camera::convert_packed_to_bgr(const GX_FRAME_BUFFER& frame, bool color, bool ten_bit, std::string* error) {
  VxInt32 dx;
  if (ten_bit)
    dx = DxRaw10PackedToRaw16(frame.pImgBuf, raw16_buffer_.data(),
                               static_cast<VxUint32>(frame.nWidth), static_cast<VxUint32>(frame.nHeight));
  else
    dx = DxRaw12PackedToRaw16(frame.pImgBuf, raw16_buffer_.data(),
                               static_cast<VxUint32>(frame.nWidth), static_cast<VxUint32>(frame.nHeight));
  if (dx != DX_OK) { *error = "Packed conversion failed: " + std::to_string(dx); return false; }
  return convert_raw16_to_bgr(raw16_buffer_.data(), frame.nWidth, frame.nHeight, color,
                               ten_bit ? DX_BIT_2_9 : DX_BIT_4_11, error);
}

#endif  // DOORLOCK_FAKE_ONLY
