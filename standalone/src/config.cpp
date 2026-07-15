#include "config.hpp"
#include "log.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

Config Config::load(int argc, char** argv) {
  Config cfg;
  std::string config_path;

  // Parse CLI: --config <path> and --set key=value
  std::vector<std::pair<std::string, std::string>> overrides;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (std::strncmp(argv[i], "--set", 5) == 0 && i + 1 < argc) {
      std::string kv = argv[++i];
      auto eq = kv.find('=');
      if (eq != std::string::npos) {
        overrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
      }
    }
  }

  // Load YAML if provided
  if (!config_path.empty()) {
    try {
      YAML::Node root = YAML::LoadFile(config_path);

      if (auto n = root["camera"]) {
        if (n["exposure_time"]) cfg.camera.exposure_time = n["exposure_time"].as<double>();
        if (n["gain"]) cfg.camera.gain = n["gain"].as<double>();
        if (n["gamma"]) cfg.camera.gamma = n["gamma"].as<double>();
        if (n["saturation"]) cfg.camera.saturation = n["saturation"].as<int>();
        if (n["contrast"]) cfg.camera.contrast = n["contrast"].as<double>();
        if (n["fake"]) cfg.camera.fake = n["fake"].as<bool>();
      }

      if (auto n = root["encoder"]) {
        if (n["crop_size"]) cfg.encoder.crop_size = n["crop_size"].as<int>();
        if (n["output_size"]) cfg.encoder.output_size = n["output_size"].as<int>();
        if (n["output_fps"]) cfg.encoder.output_fps = n["output_fps"].as<int>();
        if (n["target_bitrate"]) cfg.encoder.target_bitrate = n["target_bitrate"].as<int>();
        if (n["x264_preset"]) cfg.encoder.x264_preset = n["x264_preset"].as<std::string>();
        if (n["intra_only"]) cfg.encoder.intra_only = n["intra_only"].as<bool>();
        if (n["keyframe_interval"]) cfg.encoder.keyframe_interval = n["keyframe_interval"].as<int>();
        if (n["h264_slices"]) cfg.encoder.h264_slices = n["h264_slices"].as<int>();
        if (n["slice_max_bytes"]) cfg.encoder.slice_max_bytes = n["slice_max_bytes"].as<int>();
        if (n["static_simplify"]) cfg.encoder.static_simplify = n["static_simplify"].as<bool>();
        if (n["motion_threshold"]) cfg.encoder.motion_threshold = n["motion_threshold"].as<int>();
        if (n["motion_erode_px"]) cfg.encoder.motion_erode_px = n["motion_erode_px"].as<int>();
        if (n["motion_dilate_px"]) cfg.encoder.motion_dilate_px = n["motion_dilate_px"].as<int>();
        if (n["motion_trail_frames"]) cfg.encoder.motion_trail_frames = n["motion_trail_frames"].as<int>();
        if (n["trail_disable_motion_ratio"]) cfg.encoder.trail_disable_motion_ratio = n["trail_disable_motion_ratio"].as<double>();
        if (n["bg_update_alpha"]) cfg.encoder.bg_update_alpha = n["bg_update_alpha"].as<double>();
        if (n["bg_blur_sigma"]) cfg.encoder.bg_blur_sigma = n["bg_blur_sigma"].as<double>();
        if (n["center_clear_size"]) cfg.encoder.center_clear_size = n["center_clear_size"].as<int>();
        if (n["center_smooth_sigma"]) cfg.encoder.center_smooth_sigma = n["center_smooth_sigma"].as<double>();
        if (n["center_color_gate"]) cfg.encoder.center_color_gate = n["center_color_gate"].as<bool>();
        if (n["center_color_min_saturation"]) cfg.encoder.center_color_min_saturation = n["center_color_min_saturation"].as<int>();
        if (n["center_color_min_value"]) cfg.encoder.center_color_min_value = n["center_color_min_value"].as<int>();
        if (n["center_color_red_min_saturation"]) cfg.encoder.center_color_red_min_saturation = n["center_color_red_min_saturation"].as<int>();
        if (n["center_color_green_min_saturation"]) cfg.encoder.center_color_green_min_saturation = n["center_color_green_min_saturation"].as<int>();
        if (n["center_color_blue_min_saturation"]) cfg.encoder.center_color_blue_min_saturation = n["center_color_blue_min_saturation"].as<int>();
        if (n["center_color_morph_px"]) cfg.encoder.center_color_morph_px = n["center_color_morph_px"].as<int>();
        if (n["center_color_temporal_window"]) cfg.encoder.center_color_temporal_window = n["center_color_temporal_window"].as<int>();
        if (n["center_color_persist_frames"]) cfg.encoder.center_color_persist_frames = n["center_color_persist_frames"].as<int>();
        if (n["motion_adaptive_smooth_enable"]) cfg.encoder.motion_adaptive_smooth_enable = n["motion_adaptive_smooth_enable"].as<bool>();
        if (n["motion_adaptive_smooth_sigma"]) cfg.encoder.motion_adaptive_smooth_sigma = n["motion_adaptive_smooth_sigma"].as<double>();
        if (n["motion_adaptive_ratio_threshold"]) cfg.encoder.motion_adaptive_ratio_threshold = n["motion_adaptive_ratio_threshold"].as<double>();
        if (n["force_monochrome"]) cfg.encoder.force_monochrome = n["force_monochrome"].as<bool>();
      }

      if (auto n = root["serial"]) {
        if (n["port"]) cfg.serial.port = n["port"].as<std::string>();
        if (n["baud_rate"]) cfg.serial.baud_rate = n["baud_rate"].as<int>();
        if (n["flush_interval_ms"]) cfg.serial.flush_interval_ms = n["flush_interval_ms"].as<int>();
        if (n["reconnect_interval_ms"]) cfg.serial.reconnect_interval_ms = n["reconnect_interval_ms"].as<int>();
        if (n["max_backlog_bytes"]) cfg.serial.max_backlog_bytes = n["max_backlog_bytes"].as<size_t>();
        if (n["fake"]) cfg.serial.fake = n["fake"].as<bool>();
        if (n["transport_mode"]) cfg.serial.transport_mode = n["transport_mode"].as<std::string>();
      }

      if (auto n = root["bandwidth"]) {
        if (n["limit_kbytes_per_s"]) cfg.bandwidth.limit_kbytes_per_s = n["limit_kbytes_per_s"].as<double>();
        if (n["window_s"]) cfg.bandwidth.window_s = n["window_s"].as<double>();
        if (n["max_tx_delay_s"]) cfg.bandwidth.max_tx_delay_s = n["max_tx_delay_s"].as<double>();
      }

      if (auto n = root["display"]) {
        if (n["enable"]) cfg.display.enable = n["enable"].as<bool>();
      }

      if (auto n = root["debug"]) {
        if (n["dump_enable"]) cfg.debug.dump_enable = n["dump_enable"].as<bool>();
        if (n["dump_every_n_frames"]) cfg.debug.dump_every_n_frames = n["dump_every_n_frames"].as<int>();
        if (n["dump_dir"]) cfg.debug.dump_dir = n["dump_dir"].as<std::string>();
        if (n["dump_save_raw"]) cfg.debug.dump_save_raw = n["dump_save_raw"].as<bool>();
        if (n["dump_save_roi"]) cfg.debug.dump_save_roi = n["dump_save_roi"].as<bool>();
        if (n["dump_save_static"]) cfg.debug.dump_save_static = n["dump_save_static"].as<bool>();
        if (n["dump_save_final"]) cfg.debug.dump_save_final = n["dump_save_final"].as<bool>();
      }

      LOG_INFO("Loaded config from %s", config_path.c_str());
    } catch (const std::exception& ex) {
      LOG_ERROR("Failed to load config %s: %s", config_path.c_str(), ex.what());
    }
  }

  // Apply --set overrides
  for (auto& [key, val] : overrides) {
    if (key == "camera.exposure_time") cfg.camera.exposure_time = std::stod(val);
    else if (key == "camera.gain") cfg.camera.gain = std::stod(val);
    else if (key == "camera.gamma") cfg.camera.gamma = std::stod(val);
    else if (key == "camera.saturation") cfg.camera.saturation = std::stoi(val);
    else if (key == "camera.contrast") cfg.camera.contrast = std::stod(val);
    else if (key == "camera.fake") cfg.camera.fake = (val == "true" || val == "1");
    else if (key == "encoder.crop_size") cfg.encoder.crop_size = std::stoi(val);
    else if (key == "encoder.output_size") cfg.encoder.output_size = std::stoi(val);
    else if (key == "encoder.output_fps") cfg.encoder.output_fps = std::stoi(val);
    else if (key == "encoder.target_bitrate") cfg.encoder.target_bitrate = std::stoi(val);
    else if (key == "encoder.x264_preset") cfg.encoder.x264_preset = val;
    else if (key == "encoder.intra_only") cfg.encoder.intra_only = (val == "true" || val == "1");
    else if (key == "encoder.keyframe_interval") cfg.encoder.keyframe_interval = std::stoi(val);
    else if (key == "encoder.h264_slices") cfg.encoder.h264_slices = std::stoi(val);
    else if (key == "encoder.slice_max_bytes") cfg.encoder.slice_max_bytes = std::stoi(val);
    else if (key == "encoder.static_simplify") cfg.encoder.static_simplify = (val == "true" || val == "1");
    else if (key == "encoder.center_color_gate") cfg.encoder.center_color_gate = (val == "true" || val == "1");
    else if (key == "encoder.center_color_min_saturation") cfg.encoder.center_color_min_saturation = std::stoi(val);
    else if (key == "encoder.center_color_min_value") cfg.encoder.center_color_min_value = std::stoi(val);
    else if (key == "encoder.center_color_red_min_saturation") cfg.encoder.center_color_red_min_saturation = std::stoi(val);
    else if (key == "encoder.center_color_green_min_saturation") cfg.encoder.center_color_green_min_saturation = std::stoi(val);
    else if (key == "encoder.center_color_blue_min_saturation") cfg.encoder.center_color_blue_min_saturation = std::stoi(val);
    else if (key == "encoder.center_color_morph_px") cfg.encoder.center_color_morph_px = std::stoi(val);
    else if (key == "encoder.center_color_temporal_window") cfg.encoder.center_color_temporal_window = std::stoi(val);
    else if (key == "encoder.center_color_persist_frames") cfg.encoder.center_color_persist_frames = std::stoi(val);
    else if (key == "encoder.motion_adaptive_smooth_enable") cfg.encoder.motion_adaptive_smooth_enable = (val == "true" || val == "1");
    else if (key == "encoder.motion_adaptive_smooth_sigma") cfg.encoder.motion_adaptive_smooth_sigma = std::stod(val);
    else if (key == "encoder.motion_adaptive_ratio_threshold") cfg.encoder.motion_adaptive_ratio_threshold = std::stod(val);
    else if (key == "encoder.center_smooth_sigma") cfg.encoder.center_smooth_sigma = std::stod(val);
    else if (key == "encoder.force_monochrome") cfg.encoder.force_monochrome = (val == "true" || val == "1");
    else if (key == "serial.port") cfg.serial.port = val;
    else if (key == "serial.baud_rate") cfg.serial.baud_rate = std::stoi(val);
    else if (key == "serial.flush_interval_ms") cfg.serial.flush_interval_ms = std::stoi(val);
    else if (key == "serial.max_backlog_bytes") cfg.serial.max_backlog_bytes = static_cast<size_t>(std::stoul(val));
    else if (key == "serial.fake") cfg.serial.fake = (val == "true" || val == "1");
    else if (key == "serial.transport_mode") cfg.serial.transport_mode = val;
    else if (key == "bandwidth.limit_kbytes_per_s") cfg.bandwidth.limit_kbytes_per_s = std::stod(val);
    else if (key == "bandwidth.window_s") cfg.bandwidth.window_s = std::stod(val);
    else if (key == "bandwidth.max_tx_delay_s") cfg.bandwidth.max_tx_delay_s = std::stod(val);
    else if (key == "display.enable") cfg.display.enable = (val == "true" || val == "1");
    else LOG_WARN("Unknown override key: %s", key.c_str());
  }

  cfg.validate();
  return cfg;
}

void Config::validate() {
  auto clamp = [](auto& v, auto lo, auto hi, const char* name) {
    if (v < lo) { LOG_WARN("%s=%d too low, clamped to %d", name, (int)v, (int)lo); v = lo; }
    if (v > hi) { LOG_WARN("%s=%d too high, clamped to %d", name, (int)v, (int)hi); v = hi; }
  };
  auto normalize_transport_mode = [](std::string* mode) {
    if (mode == nullptr) return;
    std::transform(mode->begin(), mode->end(), mode->begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (*mode == "chunk_v1" || *mode == "legacy" || *mode == "legacy_chunk") {
      *mode = "legacy_chunk_v1";
      return;
    }
    if (*mode == "raw" || *mode == "raw_annexb" || *mode == "raw_h264_annexb") {
      *mode = "raw_h264";
      return;
    }
    if (*mode != "raw_h264" && *mode != "legacy_chunk_v1") {
      LOG_WARN("serial.transport_mode=%s invalid, fallback to raw_h264", mode->c_str());
      *mode = "raw_h264";
    }
  };

  clamp(encoder.output_fps, 1, 60, "output_fps");
  clamp(encoder.keyframe_interval, 1, 600, "keyframe_interval");
  clamp(encoder.h264_slices, 1, 16, "h264_slices");
  clamp(encoder.slice_max_bytes, 0, 1500, "slice_max_bytes");
  clamp(encoder.motion_trail_frames, 0, 15, "motion_trail_frames");
  clamp(encoder.motion_erode_px, 0, 20, "motion_erode_px");
  clamp(encoder.motion_dilate_px, 0, 20, "motion_dilate_px");
  clamp(encoder.center_color_min_saturation, 0, 255, "center_color_min_saturation");
  clamp(encoder.center_color_min_value, 0, 255, "center_color_min_value");
  clamp(encoder.center_color_red_min_saturation, 0, 255, "center_color_red_min_saturation");
  clamp(encoder.center_color_green_min_saturation, 0, 255, "center_color_green_min_saturation");
  clamp(encoder.center_color_blue_min_saturation, 0, 255, "center_color_blue_min_saturation");
  clamp(encoder.center_color_morph_px, 0, 6, "center_color_morph_px");
  clamp(encoder.center_color_temporal_window, 1, 15, "center_color_temporal_window");
  clamp(encoder.center_color_persist_frames, 0, 30, "center_color_persist_frames");
  if (encoder.motion_adaptive_smooth_sigma < 0.0) encoder.motion_adaptive_smooth_sigma = 0.0;
  if (encoder.motion_adaptive_smooth_sigma > 5.0) encoder.motion_adaptive_smooth_sigma = 5.0;
  if (encoder.motion_adaptive_ratio_threshold < 0.0) encoder.motion_adaptive_ratio_threshold = 0.0;
  if (encoder.motion_adaptive_ratio_threshold > 1.0) encoder.motion_adaptive_ratio_threshold = 1.0;
  if (camera.exposure_time < 1000.0) camera.exposure_time = 1000.0;
  if (camera.gamma < 0.1) camera.gamma = 0.1;
  if (camera.gamma > 4.0) camera.gamma = 4.0;
  if (camera.contrast < 0.1) camera.contrast = 0.1;
  if (camera.contrast > 4.0) camera.contrast = 4.0;
  clamp(camera.saturation, 0, 128, "saturation");

  if (encoder.trail_disable_motion_ratio < 0.0) encoder.trail_disable_motion_ratio = 0.0;
  if (encoder.trail_disable_motion_ratio > 1.0) encoder.trail_disable_motion_ratio = 1.0;
  if (encoder.center_smooth_sigma < 0.0) encoder.center_smooth_sigma = 0.0;
  if (encoder.center_smooth_sigma > 3.0) encoder.center_smooth_sigma = 3.0;
  if (bandwidth.limit_kbytes_per_s < 1.0) {
    LOG_WARN("bandwidth.limit_kbytes_per_s=%.2f too low, clamped to 1.0", bandwidth.limit_kbytes_per_s);
    bandwidth.limit_kbytes_per_s = 1.0;
  }
  if (bandwidth.window_s < 0.2) bandwidth.window_s = 0.2;
  if (bandwidth.max_tx_delay_s < 0.05) bandwidth.max_tx_delay_s = 0.05;
  if (serial.flush_interval_ms < 20) {
    LOG_WARN("flush_interval_ms=%d too low, clamped to 20", serial.flush_interval_ms);
    serial.flush_interval_ms = 20;
  }
  if (serial.reconnect_interval_ms < 100) serial.reconnect_interval_ms = 100;
  normalize_transport_mode(&serial.transport_mode);
  if (debug.dump_every_n_frames < 1) debug.dump_every_n_frames = 1;
}
