#include "display.hpp"
#include "log.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>

Display::Display(const Config& cfg, DisplayExchange& display_in, std::atomic<bool>& running)
    : cfg_(cfg), display_in_(display_in), running_(running) {}

void Display::run() {
  cv::namedWindow("Doorlock Sniper Raw", cv::WINDOW_NORMAL);
  cv::namedWindow("Doorlock Sniper ROI", cv::WINDOW_NORMAL);
  cv::namedWindow("Doorlock Sniper Static", cv::WINDOW_NORMAL);
  cv::namedWindow("Doorlock Sniper", cv::WINDOW_NORMAL);
  cv::setWindowProperty("Doorlock Sniper Raw", cv::WND_PROP_ASPECT_RATIO, cv::WINDOW_KEEPRATIO);
  cv::resizeWindow("Doorlock Sniper ROI", cfg_.encoder.output_size, cfg_.encoder.output_size);
  cv::resizeWindow("Doorlock Sniper Static", cfg_.encoder.output_size, cfg_.encoder.output_size);
  cv::resizeWindow("Doorlock Sniper", cfg_.encoder.output_size, cfg_.encoder.output_size);

  // Create debug dump directory if needed
  if (cfg_.debug.dump_enable) {
    std::filesystem::path dump_dir = std::filesystem::path(cfg_.debug.dump_dir) / "encoder";
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    if (ec) {
      LOG_WARN("Cannot create debug dump dir: %s (%s)", dump_dir.string().c_str(), ec.message().c_str());
    }
  }

  LOG_INFO("Display thread started");

  DisplayFrames frames;
  while (running_.load(std::memory_order_acquire)) {
    if (display_in_.read(frames)) {
      if (!frames.raw.empty())
        cv::imshow("Doorlock Sniper Raw", frames.raw);
      if (!frames.roi.empty())
        cv::imshow("Doorlock Sniper ROI", frames.roi);
      if (!frames.static_frame.empty())
        cv::imshow("Doorlock Sniper Static", frames.static_frame);
      if (!frames.final_frame.empty())
        cv::imshow("Doorlock Sniper", frames.final_frame);

      // Debug dump
      if (cfg_.debug.dump_enable && !frames.final_frame.empty()) {
        frame_counter_++;
        if ((frame_counter_ % static_cast<uint64_t>(cfg_.debug.dump_every_n_frames)) == 0U) {
          std::filesystem::path dump_dir = std::filesystem::path(cfg_.debug.dump_dir) / "encoder";
          std::ostringstream idx;
          idx << std::setw(8) << std::setfill('0') << frame_counter_;
          std::string fid = idx.str();
          if (cfg_.debug.dump_save_raw && !frames.raw.empty())
            cv::imwrite((dump_dir / ("raw_" + fid + ".png")).string(), frames.raw);
          if (cfg_.debug.dump_save_roi && !frames.roi.empty())
            cv::imwrite((dump_dir / ("roi_" + fid + ".png")).string(), frames.roi);
          if (cfg_.debug.dump_save_static && !frames.static_frame.empty())
            cv::imwrite((dump_dir / ("static_" + fid + ".png")).string(), frames.static_frame);
          if (cfg_.debug.dump_save_final)
            cv::imwrite((dump_dir / ("final_" + fid + ".png")).string(), frames.final_frame);
        }
      }
    }

    int key = cv::waitKey(1);
    if (key == 'q' || key == 27) {
      running_.store(false, std::memory_order_release);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  cv::destroyAllWindows();
  LOG_INFO("Display thread stopped");
}
