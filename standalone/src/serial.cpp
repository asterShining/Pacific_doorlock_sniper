#include "serial.hpp"
#include "log.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

constexpr tcflag_t kRawModeMask =
    static_cast<tcflag_t>(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);

speed_t map_baud_rate(int baud_rate) {
  switch (baud_rate) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:
      LOG_ERROR("Unsupported baud rate: %d", baud_rate);
      return B921600;
  }
}

}  // namespace

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string& port, int baud_rate) {
  if (fake_) {
    LOG_INFO("Fake serial mode active (no real port)");
    return true;
  }

  if (fd_ >= 0) return true;

  const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
  if (fd < 0) {
    LOG_WARN("open(%s) failed: %s", port.c_str(), std::strerror(errno));
    return false;
  }

  termios tty{};
  if (tcgetattr(fd, &tty) != 0) {
    LOG_ERROR("tcgetattr(%s) failed: %s", port.c_str(), std::strerror(errno));
    ::close(fd);
    return false;
  }

  cfmakeraw(&tty);
  tty.c_iflag &= ~kRawModeMask;
  tty.c_oflag = 0;
  tty.c_lflag = 0;
  tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
  tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | PARODD | CSTOPB | CRTSCTS);
  tty.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
  tty.c_cflag |= CS8;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1;

  speed_t speed = map_baud_rate(baud_rate);
  if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0) {
    LOG_ERROR("cfsetspeed failed for baud=%d: %s", baud_rate, std::strerror(errno));
    ::close(fd);
    return false;
  }

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    LOG_ERROR("tcsetattr(%s) failed: %s", port.c_str(), std::strerror(errno));
    ::close(fd);
    return false;
  }

  tcflush(fd, TCIOFLUSH);
  fd_ = fd;
  LOG_INFO("Serial port ready: %s @ %d", port.c_str(), baud_rate);
  return true;
}

void SerialPort::close() {
  if (fake_) return;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool SerialPort::write_all(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0u) return false;

  if (fake_) {
    std::lock_guard<std::mutex> lock(fake_mutex_);
    fake_queue_.emplace(data, data + size);
    fake_cv_.notify_one();
    return true;
  }

  if (fd_ < 0) return false;

  size_t written_total = 0u;
  while (written_total < size) {
    const ssize_t written = ::write(fd_, data + written_total, size - written_total);
    if (written < 0) {
      if (errno == EINTR) continue;
      LOG_ERROR("Serial write failed: %s", std::strerror(errno));
      return false;
    }
    if (written == 0) {
      LOG_ERROR("Serial write returned 0 unexpectedly");
      return false;
    }
    written_total += static_cast<size_t>(written);
  }
  return true;
}

bool SerialPort::read_chunk(std::vector<uint8_t>& out, int timeout_ms) {
  std::unique_lock<std::mutex> lock(fake_mutex_);
  if (fake_queue_.empty()) {
    fake_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
  }
  if (fake_queue_.empty()) return false;
  out = std::move(fake_queue_.front());
  fake_queue_.pop();
  return true;
}
