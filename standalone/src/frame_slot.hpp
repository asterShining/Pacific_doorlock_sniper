#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <opencv2/core.hpp>

// SPSC "latest frame" slot: producer always writes the newest frame,
// consumer always reads the newest available frame. Frames in between are skipped.
struct alignas(64) FrameSlot {
  cv::Mat frame;
  std::atomic<uint64_t> seq{0};
};

class FrameExchange {
 public:
  // Producer: write a new frame
  void write(const cv::Mat& src) {
    int idx = write_idx_.load(std::memory_order_relaxed);
    src.copyTo(slots_[idx].frame);
    slots_[idx].seq.store(++write_seq_, std::memory_order_release);
    write_idx_.store(idx ^ 1, std::memory_order_release);
    cv_.notify_one();
  }

  // Consumer: read the latest frame if newer than last read.
  // Returns true if a new frame was read, false on timeout.
  bool read(cv::Mat& dst, std::chrono::milliseconds timeout) {
    // Check both slots, pick the one with higher seq
    int best = -1;
    uint64_t best_seq = read_seq_;

    for (int i = 0; i < 2; ++i) {
      uint64_t s = slots_[i].seq.load(std::memory_order_acquire);
      if (s > best_seq) {
        best_seq = s;
        best = i;
      }
    }

    if (best >= 0) {
      slots_[best].frame.copyTo(dst);
      read_seq_ = best_seq;
      return true;
    }

    // Wait for notification
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, timeout);
    }

    // Try again after wakeup
    for (int i = 0; i < 2; ++i) {
      uint64_t s = slots_[i].seq.load(std::memory_order_acquire);
      if (s > best_seq) {
        best_seq = s;
        best = i;
      }
    }

    if (best >= 0) {
      slots_[best].frame.copyTo(dst);
      read_seq_ = best_seq;
      return true;
    }

    return false;
  }

 private:
  alignas(64) FrameSlot slots_[2];
  alignas(64) std::atomic<int> write_idx_{0};
  alignas(64) uint64_t write_seq_ = 0;
  uint64_t read_seq_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// For display: 4 frames (raw, roi, static, final)
struct DisplayFrames {
  cv::Mat raw;
  cv::Mat roi;
  cv::Mat static_frame;
  cv::Mat final_frame;
};

struct alignas(64) DisplaySlot {
  DisplayFrames frames;
  std::atomic<uint64_t> seq{0};
};

class DisplayExchange {
 public:
  void write(const DisplayFrames& src) {
    int idx = write_idx_.load(std::memory_order_relaxed);
    src.raw.copyTo(slots_[idx].frames.raw);
    src.roi.copyTo(slots_[idx].frames.roi);
    src.static_frame.copyTo(slots_[idx].frames.static_frame);
    src.final_frame.copyTo(slots_[idx].frames.final_frame);
    slots_[idx].seq.store(++write_seq_, std::memory_order_release);
    write_idx_.store(idx ^ 1, std::memory_order_release);
  }

  bool read(DisplayFrames& dst) {
    int best = -1;
    uint64_t best_seq = read_seq_;
    for (int i = 0; i < 2; ++i) {
      uint64_t s = slots_[i].seq.load(std::memory_order_acquire);
      if (s > best_seq) {
        best_seq = s;
        best = i;
      }
    }
    if (best < 0) return false;

    slots_[best].frames.raw.copyTo(dst.raw);
    slots_[best].frames.roi.copyTo(dst.roi);
    slots_[best].frames.static_frame.copyTo(dst.static_frame);
    slots_[best].frames.final_frame.copyTo(dst.final_frame);
    read_seq_ = best_seq;
    return true;
  }

 private:
  alignas(64) DisplaySlot slots_[2];
  alignas(64) std::atomic<int> write_idx_{0};
  alignas(64) uint64_t write_seq_ = 0;
  uint64_t read_seq_ = 0;
};
