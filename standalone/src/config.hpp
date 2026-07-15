#pragma once

#include <string>

struct Config {
  struct Camera {
    double exposure_time = 15000.0;
    double gain = 10.0;
    double gamma = 1.25;
    int saturation = 36;
    double contrast = 0.90;
    bool fake = false;
  } camera;

  struct Encoder {
    int crop_size = 800;
    int output_size = 192;
    int output_fps = 12;
    int target_bitrate = 96;
    std::string x264_preset = "veryslow";
    bool intra_only = true;
    int keyframe_interval = 15;
    int h264_slices = 8;
    int slice_max_bytes = 240;
    bool static_simplify = true;
    int motion_threshold = 20;
    int motion_erode_px = 2;
    int motion_dilate_px = 1;
    int motion_trail_frames = 1;
    double trail_disable_motion_ratio = 0.06;
    double bg_update_alpha = 0.006;
    double bg_blur_sigma = 2.4;
    int center_clear_size = 96;
    double center_smooth_sigma = 0.6;
    bool center_color_gate = true;
    int center_color_min_saturation = 60;
    int center_color_min_value = 45;
    int center_color_red_min_saturation = 78;
    int center_color_green_min_saturation = 65;
    int center_color_blue_min_saturation = 78;
    int center_color_morph_px = 1;
    int center_color_temporal_window = 2;
    int center_color_persist_frames = 2;
    bool motion_adaptive_smooth_enable = false;
    double motion_adaptive_smooth_sigma = 1.5;
    double motion_adaptive_ratio_threshold = 0.10;
    bool force_monochrome = false;
  } encoder;

  struct Serial {
    std::string port = "/dev/ttyACM0";
    int baud_rate = 921600;
    int flush_interval_ms = 20;
    int reconnect_interval_ms = 1000;
    size_t max_backlog_bytes = 3000;
    bool fake = false;
    std::string transport_mode = "raw_h264";
  } serial;

  struct Bandwidth {
    double limit_kbytes_per_s = 14.0;
    double window_s = 2.0;
    double max_tx_delay_s = 0.25;
  } bandwidth;

  struct Display {
    bool enable = true;
  } display;

  struct Debug {
    bool dump_enable = false;
    int dump_every_n_frames = 20;
    std::string dump_dir = "sniper_debug_imgs";
    bool dump_save_raw = true;
    bool dump_save_roi = true;
    bool dump_save_static = true;
    bool dump_save_final = true;
  } debug;

  static Config load(int argc, char** argv);
  void validate();
};
