#include "pipeline.hpp"
#include "crc16.hpp"
#include "log.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

static int64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

Pipeline::Pipeline(const Config& cfg, FrameExchange& frame_in, DisplayExchange& display_out,
                   SerialPort& serial, std::atomic<bool>& running)
    : cfg_(cfg), frame_in_(frame_in), display_out_(display_out),
      serial_(serial), running_(running) {
  raw_tx_buffer_.reserve(2048);
}

Pipeline::~Pipeline() {
  shutdown_gstreamer();
}

void Pipeline::run() {
  init_gstreamer();

  // Try to open serial port at startup
  serial_.open(cfg_.serial.port, cfg_.serial.baud_rate);
  pending_stream_reset_ = true;
  last_reconnect_ns_ = now_ns();
  last_raw_credit_ns_ = last_reconnect_ns_;
  raw_tx_credit_bytes_ = std::max(1.0, cfg_.bandwidth.limit_kbytes_per_s * cfg_.serial.flush_interval_ms);

  LOG_INFO("Pipeline started: crop=%d -> %dx%d@%dfps %dkbps, tx_limit=%.2fkB/s transport=%s",
           cfg_.encoder.crop_size, cfg_.encoder.output_size, cfg_.encoder.output_size,
           cfg_.encoder.output_fps, cfg_.encoder.target_bitrate,
           cfg_.bandwidth.limit_kbytes_per_s, cfg_.serial.transport_mode.c_str());

  cv::Mat input_frame;
  const int64_t frame_interval_ns = 1000000000LL / std::max(cfg_.encoder.output_fps, 1);

  while (running_.load(std::memory_order_acquire)) {
    // Read latest frame from camera (20ms timeout)
    if (!frame_in_.read(input_frame, std::chrono::milliseconds(20))) {
      // No new frame, but still try to flush serial
      pull_and_transmit();
      continue;
    }

    // Frame rate limiting (only if < 60fps requested)
    if (cfg_.encoder.output_fps < 60) {
      int64_t t = now_ns();
      if (last_encode_stamp_ns_ > 0 && (t - last_encode_stamp_ns_) < frame_interval_ns)
        continue;
      last_encode_stamp_ns_ = t;
    }

    // Preprocess
    cv::Mat roi_frame, static_frame;
    cv::Mat processed = preprocess_image(input_frame, &roi_frame, &static_frame);

    // Update display exchange
    if (cfg_.display.enable) {
      DisplayFrames df;
      cv::resize(input_frame, df.raw,
                 cv::Size(std::max(1, input_frame.cols / 2), std::max(1, input_frame.rows / 2)),
                 0, 0, cv::INTER_AREA);
      df.roi = roi_frame;
      df.static_frame = static_frame;
      df.final_frame = processed;
      display_out_.write(df);
    }

    // Encode and transmit
    push_frame_to_gstreamer(processed);
    pull_and_transmit();

    frame_count_++;
  }

  LOG_INFO("Pipeline stopped, %u frames processed", frame_count_);
}

void Pipeline::init_gstreamer() {
  gst_init(nullptr, nullptr);

  pipeline_ = gst_pipeline_new("encoder_pipe");
  appsrc_ = gst_element_factory_make("appsrc", "source");
  appsink_ = gst_element_factory_make("appsink", "sink");
  GstElement* convert = gst_element_factory_make("videoconvert", "convert");
  GstElement* encoder = gst_element_factory_make("x264enc", "encoder");
  GstElement* parser = gst_element_factory_make("h264parse", "parser");

  if (!pipeline_ || !appsrc_ || !appsink_ || !convert || !encoder) {
    LOG_FATAL("GStreamer element creation failed");
    return;
  }

  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw",
      "format", G_TYPE_STRING, "BGR",
      "width", G_TYPE_INT, cfg_.encoder.output_size,
      "height", G_TYPE_INT, cfg_.encoder.output_size,
      "framerate", GST_TYPE_FRACTION, cfg_.encoder.output_fps, 1,
      nullptr);
  g_object_set(G_OBJECT(appsrc_),
      "caps", caps, "stream-type", 0, "format", GST_FORMAT_TIME,
      "is-live", TRUE, "do-timestamp", TRUE, nullptr);
  gst_caps_unref(caps);

  // x264 configuration
  // Keep the "low bitrate" lane slightly wider in the real hardware path so that
  // 88-100kbps presets still get the more recovery-friendly encoder knobs.
  const bool low_bitrate = (cfg_.encoder.target_bitrate <= 104);
  // Use independent frames for the fragile low-rate link when requested; this
  // costs quality, but damaged chunks stop at the current frame instead of
  // poisoning the center view for multiple frames.
  const int key_int_max = cfg_.encoder.intra_only ? 1 : std::max(1, cfg_.encoder.keyframe_interval);
  const int h264_slices = std::clamp(cfg_.encoder.h264_slices, 1, 16);
  const int default_speed = low_bitrate ? 9 : 3;
  int speed_preset = default_speed;

  std::string preset_lower = cfg_.encoder.x264_preset;
  std::transform(preset_lower.begin(), preset_lower.end(), preset_lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (!preset_lower.empty() && preset_lower != "auto") {
    if (preset_lower == "ultrafast") speed_preset = 1;
    else if (preset_lower == "superfast") speed_preset = 2;
    else if (preset_lower == "veryfast") speed_preset = 3;
    else if (preset_lower == "faster") speed_preset = 4;
    else if (preset_lower == "fast") speed_preset = 5;
    else if (preset_lower == "medium") speed_preset = 6;
    else if (preset_lower == "slow") speed_preset = 7;
    else if (preset_lower == "slower") speed_preset = 8;
    else if (preset_lower == "veryslow") speed_preset = 9;
    else if (preset_lower == "placebo") speed_preset = 10;
    else {
      LOG_WARN("Unknown x264_preset='%s', using default", cfg_.encoder.x264_preset.c_str());
      speed_preset = default_speed;
    }
  }

  std::ostringstream option_stream;
  option_stream << "repeat-headers=1:scenecut=0:force-cfr=1:slices=" << h264_slices;
  if (cfg_.encoder.slice_max_bytes > 0) {
    option_stream << ":slice-max-size=" << cfg_.encoder.slice_max_bytes;
  }
  if (cfg_.encoder.intra_only) {
    option_stream << ":keyint=1:min-keyint=1";
  }
  // AQ mode=2 (auto-variance): steer bits to complex center region,
  // away from flat grayscale background — critical at 88kbps.
  // Trellis=2: full-frame RD-optimized quantization, reduces blocking artifacts.
  option_stream << ":aq-mode=2:aq-strength=1.2:trellis=2";
  const std::string x264_options = option_stream.str();

  if (low_bitrate) {
    g_object_set(G_OBJECT(encoder),
        "bitrate", cfg_.encoder.target_bitrate,
        "speed-preset", speed_preset,
        "tune", 0x00000004, "byte-stream", TRUE,
        "key-int-max", key_int_max, "bframes", 4,
        "rc-lookahead", 10, "sync-lookahead", 20,
        "sliced-threads", h264_slices > 1, "ref", 1, "aud", TRUE,
        "vbv-buf-capacity", 50, "vbv-max-rate", 120,
        "option-string", x264_options.c_str(),
        "pass", 0, nullptr);
  } else {
    g_object_set(G_OBJECT(encoder),
        "bitrate", cfg_.encoder.target_bitrate,
        "speed-preset", speed_preset,
        "tune", 0x00000004, "byte-stream", TRUE,
        "key-int-max", key_int_max, "bframes", 4,
        "rc-lookahead", 10, "sync-lookahead", 20,
        "sliced-threads", h264_slices > 1, "aud", TRUE,
        "vbv-buf-capacity", 50, "vbv-max-rate", 120,
        "option-string", x264_options.c_str(),
        "pass", 0, nullptr);
  }

  GstCaps* h264_caps = gst_caps_new_simple(
      "video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream",
      "alignment", G_TYPE_STRING, "au", nullptr);
  g_object_set(G_OBJECT(appsink_),
      "caps", h264_caps, "max-buffers", 5,
      "drop", FALSE, "emit-signals", FALSE, "sync", FALSE, nullptr);
  gst_caps_unref(h264_caps);

  if (parser) {
    g_object_set(G_OBJECT(parser), "config-interval", -1, "disable-passthrough", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline_), appsrc_, convert, encoder, parser, appsink_, nullptr);
    if (!gst_element_link_many(appsrc_, convert, encoder, parser, appsink_, nullptr)) {
      LOG_FATAL("GStreamer pipeline link failed");
      return;
    }
  } else {
    LOG_WARN("h264parse unavailable, continuing without parser");
    gst_bin_add_many(GST_BIN(pipeline_), appsrc_, convert, encoder, appsink_, nullptr);
    if (!gst_element_link_many(appsrc_, convert, encoder, appsink_, nullptr)) {
      LOG_FATAL("GStreamer pipeline link failed");
      return;
    }
  }

  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOG_FATAL("GStreamer pipeline start failed");
    return;
  }

  bus_ = gst_element_get_bus(pipeline_);
  LOG_INFO("GStreamer encoder ready (%s mode, keyint=%d, slices=%d, slice_max=%d, options=%s)",
           low_bitrate ? "low-bitrate" : "low-latency", key_int_max, h264_slices,
           cfg_.encoder.slice_max_bytes, x264_options.c_str());
}

void Pipeline::shutdown_gstreamer() {
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_) gst_object_unref(bus_);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
}

cv::Mat Pipeline::preprocess_image(const cv::Mat& input, cv::Mat* roi_out, cv::Mat* static_out) {
  int x = (input.cols - cfg_.encoder.crop_size) / 2;
  int y = (input.rows - cfg_.encoder.crop_size) / 2;
  x = std::max(0, x);
  y = std::max(0, y);
  int w = std::min(cfg_.encoder.crop_size, input.cols - x);
  int h = std::min(cfg_.encoder.crop_size, input.rows - y);

  cv::Mat cropped = input(cv::Rect(x, y, w, h));
  cv::Mat resized;
  cv::resize(cropped, resized, cv::Size(cfg_.encoder.output_size, cfg_.encoder.output_size),
             0, 0, cv::INTER_LINEAR);
  if (roi_out) resized.copyTo(*roi_out);

  cv::Mat working = resized;
  if (cfg_.encoder.force_monochrome) {
    cv::Mat gray;
    cv::cvtColor(working, gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray, working, cv::COLOR_GRAY2BGR);
  }

  if (!cfg_.encoder.static_simplify) {
    if (static_out) working.copyTo(*static_out);
    return working;
  }

  cv::Mat gray;
  cv::cvtColor(working, gray, cv::COLOR_BGR2GRAY);
  if (background_gray_f32_.empty()) {
    gray.convertTo(background_gray_f32_, CV_32F);
  }

  cv::Mat bg_u8;
  cv::convertScaleAbs(background_gray_f32_, bg_u8);

  cv::Mat diff;
  cv::absdiff(gray, bg_u8, diff);

  cv::Mat motion_mask;
  cv::threshold(diff, motion_mask, cfg_.encoder.motion_threshold, 255, cv::THRESH_BINARY);

  if (cfg_.encoder.motion_erode_px > 0) {
    if (motion_erode_kernel_.empty()) {
      int k = 2 * cfg_.encoder.motion_erode_px + 1;
      motion_erode_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }
    cv::erode(motion_mask, motion_mask, motion_erode_kernel_);
  }
  if (cfg_.encoder.motion_dilate_px > 0) {
    if (motion_dilate_kernel_.empty()) {
      int k = 2 * cfg_.encoder.motion_dilate_px + 1;
      motion_dilate_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }
    cv::dilate(motion_mask, motion_mask, motion_dilate_kernel_);
  }

  double motion_ratio = static_cast<double>(cv::countNonZero(motion_mask)) /
                         static_cast<double>(motion_mask.total());
  bool suppress_trail = (motion_ratio >= cfg_.encoder.trail_disable_motion_ratio);

  cv::Mat center_mask = cv::Mat::zeros(motion_mask.size(), CV_8UC1);
  if (cfg_.encoder.center_clear_size > 0) {
    int cs = std::min({cfg_.encoder.center_clear_size, working.cols, working.rows});
    int x0 = std::max(0, working.cols / 2 - cs / 2);
    int y0 = std::max(0, working.rows / 2 - cs / 2);
    int cw = std::min(cs, working.cols - x0);
    int ch = std::min(cs, working.rows - y0);
    cv::rectangle(center_mask, cv::Rect(x0, y0, cw, ch), cv::Scalar(255), cv::FILLED);
  }

  cv::Mat static_base = working.clone();
  if (!cfg_.encoder.force_monochrome) {
    // Match the deployment note: gray/blur static background while keeping the
    // sight-center region and compact motion cues in color.
    cv::Mat gray_bg;
    cv::cvtColor(static_base, gray_bg, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray_bg, static_base, cv::COLOR_GRAY2BGR);
  }

  cv::Mat blurred;
  cv::GaussianBlur(static_base, blurred, cv::Size(),
                   std::max(0.0, cfg_.encoder.bg_blur_sigma),
                   std::max(0.0, cfg_.encoder.bg_blur_sigma));

  cv::Mat focused = blurred.clone();
  cv::Mat color_mask = center_mask.clone();
  cv::Mat trail_mask = cv::Mat::zeros(motion_mask.size(), CV_8UC1);
  if (cfg_.encoder.center_color_gate) {
    color_mask = cv::Mat::zeros(motion_mask.size(), CV_8UC1);
  }
  // Trail: overlay historical motion maxima to reveal projectile paths.
  // Active regardless of center_color_gate — the trail pixels are the
  // brightest values across recent motion regions (same approach as 五大湖).
  if (!suppress_trail) {
    trail_mask = motion_mask.clone();
    cv::bitwise_or(color_mask, motion_mask, color_mask);
  }
  working.copyTo(focused, color_mask);
  // Motion-adaptive center smoothing: increase blur during high camera motion
  // to reduce spatial complexity that the encoder struggles with at low bitrates.
  double effective_center_smooth_sigma = cfg_.encoder.center_smooth_sigma;
  if (cfg_.encoder.motion_adaptive_smooth_enable &&
      motion_ratio >= cfg_.encoder.motion_adaptive_ratio_threshold) {
    double adapt_factor = std::min(1.0,
        (motion_ratio - cfg_.encoder.motion_adaptive_ratio_threshold) /
        (1.0 - cfg_.encoder.motion_adaptive_ratio_threshold));
    effective_center_smooth_sigma = cfg_.encoder.center_smooth_sigma +
        (cfg_.encoder.motion_adaptive_smooth_sigma - cfg_.encoder.center_smooth_sigma) * adapt_factor;
    effective_center_smooth_sigma = std::max(0.01, effective_center_smooth_sigma);
  }

  const bool has_center_mask = cv::countNonZero(center_mask) > 0;
  if (has_center_mask) {
    if (cfg_.encoder.center_color_gate) {
      cv::Mat center_base = static_base;
      cv::Mat center_smoothed;
      if (effective_center_smooth_sigma > 0.01) {
        cv::GaussianBlur(static_base, center_smoothed, cv::Size(),
                         effective_center_smooth_sigma,
                         effective_center_smooth_sigma);
        center_base = center_smoothed;
      }
      center_base.copyTo(focused, center_mask);

      if (!cfg_.encoder.force_monochrome) {
        cv::Mat hsv;
        cv::cvtColor(working, hsv, cv::COLOR_BGR2HSV);

        const int min_v = std::clamp(cfg_.encoder.center_color_min_value, 0, 255);
        const int red_min_s = std::clamp(cfg_.encoder.center_color_red_min_saturation, 0, 255);
        const int grn_min_s = std::clamp(cfg_.encoder.center_color_green_min_saturation, 0, 255);
        const int blu_min_s = std::clamp(cfg_.encoder.center_color_blue_min_saturation, 0, 255);
        cv::Mat red_low;
        cv::Mat red_high;
        cv::Mat green;
        cv::Mat blue;
        cv::Mat red;
        cv::Mat center_rgb_mask;

        cv::inRange(hsv, cv::Scalar(0, red_min_s, min_v), cv::Scalar(10, 255, 255), red_low);
        cv::inRange(hsv, cv::Scalar(170, red_min_s, min_v), cv::Scalar(180, 255, 255), red_high);
        cv::inRange(hsv, cv::Scalar(35, grn_min_s, min_v), cv::Scalar(90, 255, 255), green);
        cv::inRange(hsv, cv::Scalar(95, blu_min_s, min_v), cv::Scalar(135, 255, 255), blue);
        cv::bitwise_or(red_low, red_high, red);
        cv::bitwise_or(red, green, center_rgb_mask);
        cv::bitwise_or(center_rgb_mask, blue, center_rgb_mask);
        cv::bitwise_and(center_rgb_mask, center_mask, center_rgb_mask);

        // Temporal color mask filtering: require consensus across consecutive
        // frames to suppress HSV-detection flicker that wastes H.264 bitrate.
        const int tw = std::max(1, cfg_.encoder.center_color_temporal_window);
        const int pf = std::max(0, cfg_.encoder.center_color_persist_frames);
        const size_t max_history = static_cast<size_t>(std::max(tw, pf) + 1);

        raw_color_mask_history_.push_back(center_rgb_mask.clone());
        while (raw_color_mask_history_.size() > max_history)
          raw_color_mask_history_.pop_front();

        cv::Mat filtered_color_mask;
        if (tw <= 1 && pf <= 0) {
          filtered_color_mask = center_rgb_mask;
        } else {
          size_t hist_size = raw_color_mask_history_.size();
          if (hist_size >= static_cast<size_t>(tw)) {
            size_t start = hist_size - static_cast<size_t>(tw);
            cv::Mat consensus;
            raw_color_mask_history_[start].copyTo(consensus);
            for (size_t i = start + 1; i < hist_size; i++)
              cv::bitwise_and(consensus, raw_color_mask_history_[i], consensus);
            filtered_color_mask = consensus;

            if (pf > 0 && !active_color_mask_.empty()) {
              cv::Mat recent_any = cv::Mat::zeros(center_rgb_mask.size(), CV_8UC1);
              size_t pf_start = (hist_size > static_cast<size_t>(pf)) ? (hist_size - static_cast<size_t>(pf)) : 0;
              for (size_t i = pf_start; i < hist_size; i++)
                cv::bitwise_or(recent_any, raw_color_mask_history_[i], recent_any);
              cv::Mat persisted;
              cv::bitwise_and(active_color_mask_, recent_any, persisted);
              cv::bitwise_or(filtered_color_mask, persisted, filtered_color_mask);
            }
          } else {
            filtered_color_mask = center_rgb_mask;
          }
        }
        active_color_mask_ = filtered_color_mask;

        const int morph_px = std::clamp(cfg_.encoder.center_color_morph_px, 0, 6);
        if (morph_px > 0 && cv::countNonZero(filtered_color_mask) > 0) {
          int k = 2 * morph_px + 1;
          cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
          cv::morphologyEx(filtered_color_mask, filtered_color_mask, cv::MORPH_CLOSE, kernel);
          cv::bitwise_and(filtered_color_mask, center_mask, filtered_color_mask);
        }
        working.copyTo(focused, filtered_color_mask);
      }
    } else if (cfg_.encoder.center_smooth_sigma > 0.01) {
      // Keep the sight-center visually stable: a light denoise pass costs less bitrate
      // than sharp sensor noise, so H.264 is less likely to produce obvious center blocks.
      cv::Mat center_smoothed;
      cv::GaussianBlur(working, center_smoothed, cv::Size(),
                       cfg_.encoder.center_smooth_sigma,
                       cfg_.encoder.center_smooth_sigma);
      center_smoothed.copyTo(focused, center_mask);
    }
  }
  if (static_out) focused.copyTo(*static_out);

  // Motion trail
  if (cfg_.encoder.motion_trail_frames > 0) {
    motion_mask_history_.push_back(motion_mask.clone());
    trail_frame_history_.push_back(working.clone());
    size_t max_hist = static_cast<size_t>(cfg_.encoder.motion_trail_frames + 1);
    while (motion_mask_history_.size() > max_hist) motion_mask_history_.pop_front();
    while (trail_frame_history_.size() > max_hist) trail_frame_history_.pop_front();

    size_t hist_size = motion_mask_history_.size();
    if (!suppress_trail && hist_size > 1 && hist_size == trail_frame_history_.size()) {
      cv::Mat trail_img = working.clone();
      for (size_t i = 0; i < hist_size - 1; ++i) {
        cv::bitwise_or(trail_mask, motion_mask_history_[i], trail_mask);
        cv::max(trail_img, trail_frame_history_[i], trail_img);
      }
      trail_img.copyTo(focused, trail_mask);
    }
  } else {
    motion_mask_history_.clear();
    trail_frame_history_.clear();
  }

  cv::accumulateWeighted(gray, background_gray_f32_,
                         std::clamp(cfg_.encoder.bg_update_alpha, 0.001, 0.2));
  return focused;
}

void Pipeline::push_frame_to_gstreamer(const cv::Mat& frame) {
  if (!appsrc_ || frame.empty()) return;

  size_t size = frame.total() * frame.elemSize();
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);

  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    memcpy(map.data, frame.data, size);
    gst_buffer_unmap(buffer, &map);

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc_, "push-buffer", buffer, &ret);
    if (ret != GST_FLOW_OK) {
      LOG_WARN("Push buffer failed: %d", ret);
    }
  }
  gst_buffer_unref(buffer);
}

void Pipeline::pull_and_transmit() {
  if (!appsink_) return;

  const int64_t window_ns = static_cast<int64_t>(cfg_.bandwidth.window_s * 1e9);
  const size_t window_limit_bytes = static_cast<size_t>(
      cfg_.bandwidth.limit_kbytes_per_s * 1000.0 * cfg_.bandwidth.window_s);

  // Pull all available H.264 data from GStreamer
  while (true) {
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);
    if (!sample) break;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer) {
      GstMapInfo map;
      if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        // h264parse is configured for alignment=au, so each appsink sample is
        // already one access unit. Re-splitting by NAL holds the final slice
        // until the next frame and adds visible custom-source latency.
        stream_buf_.append(map.data, map.size);
        gst_buffer_unmap(buffer, &map);
      }
    }
    gst_sample_unref(sample);
  }

  // Try serial reconnect if needed
  if (!serial_.is_open()) {
    int64_t t = now_ns();
    if (t - last_reconnect_ns_ >= cfg_.serial.reconnect_interval_ms * 1000000LL) {
      if (serial_.open(cfg_.serial.port, cfg_.serial.baud_rate)) {
        pending_stream_reset_ = true;
        last_raw_credit_ns_ = t;
        raw_tx_credit_bytes_ = std::max(1.0, cfg_.bandwidth.limit_kbytes_per_s * cfg_.serial.flush_interval_ms);
      }
      last_reconnect_ns_ = t;
    }
    // Still do backlog management even when serial is down
    clip_backlog_if_needed();
    return;
  }

  if (cfg_.serial.transport_mode == "raw_h264") {
    transmit_raw_stream(now_ns(), window_limit_bytes);
    clip_backlog_if_needed();

    int64_t t = now_ns();
    if (t - last_telemetry_ns_ > 1000000000LL) {
      while (!sent_window_.empty() && (t - sent_window_.front().first) > window_ns) {
        sent_window_bytes_ -= sent_window_.front().second;
        sent_window_.pop_front();
      }
      double window_kb = static_cast<double>(sent_window_bytes_) / 1000.0;
      double avg_kb_s = window_kb / cfg_.bandwidth.window_s;
      LOG_INFO("TX(raw): window=%.2f/%.2fkB avg=%.2fkB/s backlog=%zuB credit=%.1fB dropped=%luB",
               window_kb, static_cast<double>(window_limit_bytes) / 1000.0,
               avg_kb_s, stream_buf_.size(), raw_tx_credit_bytes_,
               static_cast<unsigned long>(dropped_bytes_));
      last_telemetry_ns_ = t;
    }
    return;
  }

  // Bandwidth-limited transmission: directly fragment into 288-byte serial payloads
  while (stream_buf_.size() >= protocol::kChunkPayloadBytes) {
    int64_t t = now_ns();

    // Prune old entries from sliding window
    while (!sent_window_.empty() && (t - sent_window_.front().first) > window_ns) {
      sent_window_bytes_ -= sent_window_.front().second;
      sent_window_.pop_front();
    }

    if (sent_window_bytes_ + protocol::kChunkPayloadBytes > window_limit_bytes) {
      break;  // Bandwidth limit reached
    }

    uint8_t flags = pending_stream_reset_ ? protocol::kFlagStreamReset : 0u;
    build_and_send_chunk(flags);
    pending_stream_reset_ = false;

    sent_window_.emplace_back(t, protocol::kChunkPayloadBytes);
    sent_window_bytes_ += protocol::kChunkPayloadBytes;
  }

  // Handle partial data (< 288 bytes) — send what we have
  if (!stream_buf_.empty() && stream_buf_.size() < protocol::kChunkPayloadBytes) {
    int64_t t = now_ns();
    while (!sent_window_.empty() && (t - sent_window_.front().first) > window_ns) {
      sent_window_bytes_ -= sent_window_.front().second;
      sent_window_.pop_front();
    }
    if (sent_window_bytes_ + stream_buf_.size() <= window_limit_bytes) {
      uint8_t flags = pending_stream_reset_ ? protocol::kFlagStreamReset : 0u;
      build_and_send_chunk(flags);
      pending_stream_reset_ = false;
      sent_window_.emplace_back(t, stream_buf_.size());
      sent_window_bytes_ += stream_buf_.size();
    }
  }

  // Backlog clipping
  clip_backlog_if_needed();

  // Telemetry
  int64_t t = now_ns();
  if (t - last_telemetry_ns_ > 1000000000LL) {
    double window_kb = static_cast<double>(sent_window_bytes_) / 1000.0;
    double avg_kb_s = window_kb / cfg_.bandwidth.window_s;
    LOG_INFO("TX: window=%.2f/%.2fkB avg=%.2fkB/s backlog=%zuB dropped=%luB",
             window_kb, static_cast<double>(window_limit_bytes) / 1000.0,
             avg_kb_s, stream_buf_.size(), static_cast<unsigned long>(dropped_bytes_));
    last_telemetry_ns_ = t;
  }
}

void Pipeline::transmit_raw_stream(int64_t now_ns_value, size_t window_limit_bytes) {
  const int64_t window_ns = static_cast<int64_t>(cfg_.bandwidth.window_s * 1e9);
  const double bytes_per_ns = cfg_.bandwidth.limit_kbytes_per_s * 1000.0 / 1e9;
  const double max_credit_bytes = std::max(
      1.0, cfg_.bandwidth.limit_kbytes_per_s * 1000.0 * cfg_.bandwidth.max_tx_delay_s);

  if (last_raw_credit_ns_ == 0) {
    last_raw_credit_ns_ = now_ns_value;
  }

  if (now_ns_value > last_raw_credit_ns_) {
    raw_tx_credit_bytes_ += static_cast<double>(now_ns_value - last_raw_credit_ns_) * bytes_per_ns;
    if (raw_tx_credit_bytes_ > max_credit_bytes) {
      raw_tx_credit_bytes_ = max_credit_bytes;
    }
    last_raw_credit_ns_ = now_ns_value;
  }

  while (!sent_window_.empty() && (now_ns_value - sent_window_.front().first) > window_ns) {
    sent_window_bytes_ -= sent_window_.front().second;
    sent_window_.pop_front();
  }

  if (stream_buf_.empty()) {
    return;
  }

  size_t credit_bytes = static_cast<size_t>(raw_tx_credit_bytes_);
  if (credit_bytes == 0u) {
    return;
  }

  if (sent_window_bytes_ >= window_limit_bytes) {
    return;
  }

  size_t available_window_bytes = window_limit_bytes - sent_window_bytes_;
  size_t payload_len = std::min(stream_buf_.size(), std::min(credit_bytes, available_window_bytes));
  if (payload_len == 0u) {
    return;
  }

  raw_tx_buffer_.resize(payload_len);
  stream_buf_.peek(raw_tx_buffer_.data(), 0, payload_len);
  if (!serial_.write_all(raw_tx_buffer_.data(), payload_len)) {
    serial_.close();
    return;
  }

  stream_buf_.consume(payload_len);
  raw_tx_credit_bytes_ -= static_cast<double>(payload_len);
  if (raw_tx_credit_bytes_ < 0.0) {
    raw_tx_credit_bytes_ = 0.0;
  }
  sent_window_.emplace_back(now_ns_value, payload_len);
  sent_window_bytes_ += payload_len;
}

void Pipeline::build_and_send_chunk(uint8_t flags) {
  uint8_t chunk[protocol::kChunkBytes] = {};

  uint16_t payload_len = static_cast<uint16_t>(
      std::min(stream_buf_.size(), protocol::kChunkPayloadBytes));

  chunk[0] = protocol::kMagic0;
  chunk[1] = protocol::kMagic1;
  chunk[2] = protocol::kVersion;
  chunk[3] = flags;
  chunk[4] = static_cast<uint8_t>(chunk_seq_ & 0xFFu);
  chunk[5] = static_cast<uint8_t>((chunk_seq_ >> 8) & 0xFFu);
  chunk[6] = static_cast<uint8_t>((chunk_seq_ >> 16) & 0xFFu);
  chunk[7] = static_cast<uint8_t>((chunk_seq_ >> 24) & 0xFFu);
  chunk[8] = static_cast<uint8_t>(payload_len & 0xFFu);
  chunk[9] = static_cast<uint8_t>((payload_len >> 8) & 0xFFu);

  // Copy payload directly from circular buffer
  stream_buf_.peek(chunk + protocol::kHeaderBytes, 0, payload_len);
  stream_buf_.consume(payload_len);

  uint16_t crc = crc16_ccitt(chunk, 298u);
  chunk[298] = static_cast<uint8_t>(crc & 0xFFu);
  chunk[299] = static_cast<uint8_t>((crc >> 8) & 0xFFu);

  chunk_seq_++;

  if (!serial_.write_all(chunk, protocol::kChunkBytes)) {
    serial_.close();
    pending_stream_reset_ = true;
  }
}

void Pipeline::clip_backlog_if_needed() {
  size_t max_bytes = static_cast<size_t>(
      cfg_.bandwidth.limit_kbytes_per_s * 1000.0 * cfg_.bandwidth.max_tx_delay_s);
  if (max_bytes < cfg_.serial.max_backlog_bytes) max_bytes = cfg_.serial.max_backlog_bytes;

  if (stream_buf_.size() <= max_bytes) return;

  size_t target_drop = stream_buf_.size() - max_bytes;
  size_t drop_bytes = target_drop;

  // Align to next Annex-B start code
  for (size_t i = target_drop; i + 4 < stream_buf_.size(); ++i) {
    bool sc3 = (stream_buf_[i] == 0 && stream_buf_[i + 1] == 0 && stream_buf_[i + 2] == 1);
    bool sc4 = (stream_buf_[i] == 0 && stream_buf_[i + 1] == 0 &&
                stream_buf_[i + 2] == 0 && stream_buf_[i + 3] == 1);
    if (sc3 || sc4) {
      drop_bytes = i;
      break;
    }
  }

  stream_buf_.drop_front(drop_bytes);
  dropped_bytes_ += drop_bytes;
  dropped_events_++;
  pending_stream_reset_ = true;

  if (dropped_events_ % 20 == 1) {
    LOG_WARN("Backlog clipped: dropped=%zuB backlog=%zuB total=%luB events=%u",
             drop_bytes, stream_buf_.size(),
             static_cast<unsigned long>(dropped_bytes_), dropped_events_);
  }
}
