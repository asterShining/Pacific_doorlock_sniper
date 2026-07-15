#pragma once

#include "crc16.hpp"
#include "frame_slot.hpp"
#include "log.hpp"
#include "protocol.hpp"
#include "serial.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <opencv2/opencv.hpp>

#include <atomic>
#include <vector>

// Decodes 300-byte serial chunks back to video frames using GStreamer.
// Used in fake serial mode to verify the full encode->decode pipeline.
class FakeDecoder {
 public:
  FakeDecoder(const Config& cfg, SerialPort& serial, DisplayExchange& display_out,
              std::atomic<bool>& running)
      : cfg_(cfg), serial_(serial), display_out_(display_out), running_(running) {}

  ~FakeDecoder() {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
    }
  }

  void run() {
    LOG_INFO("Fake decoder started (loopback decode, transport=%s)",
             cfg_.serial.transport_mode.c_str());

    // Ensure GStreamer is initialized (Pipeline may not have started yet)
    gst_init(nullptr, nullptr);

    // Build GStreamer decode pipeline
    pipeline_ = gst_pipeline_new("decoder_pipe");
    appsrc_ = gst_element_factory_make("appsrc", "dec_src");
    GstElement* parser = gst_element_factory_make("h264parse", "dec_parser");
    GstElement* decoder = gst_element_factory_make("avdec_h264", "dec_h264");
    if (!decoder) decoder = gst_element_factory_make("openh264dec", "dec_h264");
    GstElement* convert = gst_element_factory_make("videoconvert", "dec_convert");
    appsink_ = gst_element_factory_make("appsink", "dec_sink");

    if (!pipeline_ || !appsrc_ || !appsink_ || !decoder || !convert) {
      LOG_ERROR("Fake decoder: GStreamer element creation failed");
      // Try simpler pipeline without parser
      if (parser) gst_object_unref(parser);
      return;
    }

    GstCaps* src_caps = gst_caps_new_simple(
        "video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au", nullptr);
    g_object_set(G_OBJECT(appsrc_),
        "caps", src_caps, "stream-type", 0, "format", GST_FORMAT_TIME,
        "is-live", TRUE, "do-timestamp", TRUE, nullptr);
    gst_caps_unref(src_caps);

    GstCaps* sink_caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGR", nullptr);
    g_object_set(G_OBJECT(appsink_),
        "caps", sink_caps, "max-buffers", 3,
        "drop", TRUE, "emit-signals", FALSE, "sync", FALSE, nullptr);
    gst_caps_unref(sink_caps);

    if (parser) {
      gst_bin_add_many(GST_BIN(pipeline_), appsrc_, parser, decoder, convert, appsink_, nullptr);
      if (!gst_element_link_many(appsrc_, parser, decoder, convert, appsink_, nullptr)) {
        LOG_ERROR("Fake decoder: pipeline link failed");
        return;
      }
    } else {
      gst_bin_add_many(GST_BIN(pipeline_), appsrc_, decoder, convert, appsink_, nullptr);
      if (!gst_element_link_many(appsrc_, decoder, convert, appsink_, nullptr)) {
        LOG_ERROR("Fake decoder: pipeline link failed (no parser)");
        return;
      }
    }

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      LOG_ERROR("Fake decoder: pipeline start failed");
      return;
    }

    LOG_INFO("Fake decoder GStreamer pipeline ready");

    uint32_t last_seq = 0;
    bool have_seq = false;
    uint64_t decoded_frames = 0;
    uint64_t rx_chunks = 0;

    while (running_.load(std::memory_order_acquire)) {
      std::vector<uint8_t> chunk_data;
      if (!serial_.read_chunk(chunk_data, 50)) continue;

      rx_chunks++;

      uint8_t* payload_ptr = chunk_data.data();
      size_t payload_len = chunk_data.size();
      bool need_reset = false;

      if (cfg_.serial.transport_mode == "legacy_chunk_v1") {
        if (chunk_data.size() != protocol::kChunkBytes) continue;

        if (chunk_data[0] != protocol::kMagic0 || chunk_data[1] != protocol::kMagic1) continue;

        uint16_t crc_expected = chunk_data[298] | (chunk_data[299] << 8);
        uint16_t crc_actual = crc16_ccitt(chunk_data.data(), 298);
        if (crc_expected != crc_actual) continue;

        uint8_t flags = chunk_data[3];
        uint32_t seq = chunk_data[4] | (chunk_data[5] << 8) |
                       (chunk_data[6] << 16) | (chunk_data[7] << 24);
        payload_len = static_cast<size_t>(chunk_data[8] | (chunk_data[9] << 8));
        if (payload_len > protocol::kChunkPayloadBytes) continue;

        need_reset = (flags & protocol::kFlagStreamReset) != 0;
        if (have_seq && seq != last_seq + 1) need_reset = true;
        last_seq = seq;
        have_seq = true;
        payload_ptr = chunk_data.data() + protocol::kHeaderBytes;
      }

      if (need_reset) {
        gst_element_set_state(pipeline_, GST_STATE_READY);
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
      }

      if (payload_len == 0u) continue;

      // Push H.264 data to decoder
      GstBuffer* buffer = gst_buffer_new_allocate(nullptr, payload_len, nullptr);
      GstMapInfo map;
      if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, payload_ptr, payload_len);
        gst_buffer_unmap(buffer, &map);

        GstFlowReturn ret;
        g_signal_emit_by_name(appsrc_, "push-buffer", buffer, &ret);
      }
      gst_buffer_unref(buffer);

      // Pull decoded frames
      while (true) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);
        if (!sample) break;

        GstBuffer* out_buf = gst_sample_get_buffer(sample);
        if (out_buf) {
          GstMapInfo out_map;
          if (gst_buffer_map(out_buf, &out_map, GST_MAP_READ)) {
            // Get video dimensions from caps
            GstCaps* caps = gst_sample_get_caps(sample);
            GstStructure* s = gst_caps_get_structure(caps, 0);
            int width = 0, height = 0;
            gst_structure_get_int(s, "width", &width);
            gst_structure_get_int(s, "height", &height);

            if (width > 0 && height > 0) {
              cv::Mat decoded(height, width, CV_8UC3, out_map.data);
              DisplayFrames df;
              decoded.copyTo(df.final_frame);
              df.raw = df.final_frame;
              df.roi = df.final_frame;
              df.static_frame = df.final_frame;
              display_out_.write(df);
              decoded_frames++;
            }
            gst_buffer_unmap(out_buf, &out_map);
          }
        }
        gst_sample_unref(sample);
      }

      if (decoded_frames > 0 && decoded_frames % 120 == 0) {
        LOG_INFO("Fake decoder: %lu frames decoded, rx_chunks=%lu",
                 static_cast<unsigned long>(decoded_frames),
                 static_cast<unsigned long>(rx_chunks));
      }
    }

    LOG_INFO("Fake decoder stopped, %lu frames decoded, rx_chunks=%lu",
             static_cast<unsigned long>(decoded_frames),
             static_cast<unsigned long>(rx_chunks));
  }

 private:
  const Config& cfg_;
  SerialPort& serial_;
  DisplayExchange& display_out_;
  std::atomic<bool>& running_;
  GstElement* pipeline_ = nullptr;
  GstElement* appsrc_ = nullptr;
  GstElement* appsink_ = nullptr;
};
