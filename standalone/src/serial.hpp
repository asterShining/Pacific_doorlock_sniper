#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class SerialPort {
 public:
  SerialPort() = default;
  ~SerialPort();

  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;

  void set_fake(bool fake) { fake_ = fake; }
  bool open(const std::string& port, int baud_rate);
  void close();
  bool is_open() const { return fake_ || fd_ >= 0; }
  bool write_all(const uint8_t* data, size_t size);

  // Fake mode: read chunks written by write_all()
  bool read_chunk(std::vector<uint8_t>& out, int timeout_ms = 100);

 private:
  int fd_ = -1;
  bool fake_ = false;

  // Fake serial queue
  std::mutex fake_mutex_;
  std::condition_variable fake_cv_;
  std::queue<std::vector<uint8_t>> fake_queue_;
};
