#pragma once

#include "config.hpp"
#include "frame_slot.hpp"
#include "serial.hpp"
#include "stream_buffer.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <opencv2/opencv.hpp>

#include <atomic>
#include <deque>
#include <vector>

class Pipeline {
 public:
  Pipeline(const Config& cfg, FrameExchange& frame_in, DisplayExchange& display_out,
           SerialPort& serial, std::atomic<bool>& running);
  ~Pipeline();

  void run();  // Main loop, call from dedicated thread

 private:
  void init_gstreamer();
  void shutdown_gstreamer();
  cv::Mat preprocess_image(const cv::Mat& input, cv::Mat* roi_out, cv::Mat* static_out);
  void push_frame_to_gstreamer(const cv::Mat& frame);
  void pull_and_transmit();
  void transmit_raw_stream(int64_t now_ns, size_t window_limit_bytes);
  void build_and_send_chunk(uint8_t flags);
  void clip_backlog_if_needed();

  const Config& cfg_;
  FrameExchange& frame_in_;
  DisplayExchange& display_out_;
  SerialPort& serial_;
  std::atomic<bool>& running_;

  // GStreamer
  GstElement* pipeline_ = nullptr;
  GstElement* appsrc_ = nullptr;
  GstElement* appsink_ = nullptr;
  GstBus* bus_ = nullptr;

  // Stream buffer: directly from GStreamer to serial chunks (no intermediate 150B packets)
  CircularStreamBuffer<65536> stream_buf_;

  // Bandwidth limiting
  std::deque<std::pair<int64_t, size_t>> sent_window_;
  size_t sent_window_bytes_ = 0;
  uint64_t dropped_bytes_ = 0;
  uint32_t dropped_events_ = 0;
  int64_t last_telemetry_ns_ = 0;

  // Serial chunk sequencing
  uint32_t chunk_seq_ = 0;
  bool pending_stream_reset_ = true;

  // Frame rate control
  int64_t last_encode_stamp_ns_ = 0;
  uint32_t frame_count_ = 0;

  // Serial reconnect timing
  int64_t last_reconnect_ns_ = 0;
  int64_t last_raw_credit_ns_ = 0;
  double raw_tx_credit_bytes_ = 0.0;
  std::vector<uint8_t> raw_tx_buffer_;

  // Image processing state
  cv::Mat background_gray_f32_;
  cv::Mat motion_erode_kernel_;
  cv::Mat motion_dilate_kernel_;
  std::deque<cv::Mat> motion_mask_history_;
  std::deque<cv::Mat> trail_frame_history_;
  std::deque<cv::Mat> raw_color_mask_history_;
  cv::Mat active_color_mask_;
};
