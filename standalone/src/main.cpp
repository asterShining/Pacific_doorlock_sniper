#include "camera.hpp"
#include "config.hpp"
#include "display.hpp"
#include "fake_decoder.hpp"
#include "frame_slot.hpp"
#include "log.hpp"
#include "pipeline.hpp"
#include "serial.hpp"

#include <atomic>
#include <csignal>
#include <thread>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
  (void)sig;
  g_running.store(false, std::memory_order_release);
}

int main(int argc, char** argv) {
  // Install signal handlers
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  LOG_INFO("Doorlock Sniper starting...");

  // Load config
  Config cfg = Config::load(argc, argv);

  // Shared state
  FrameExchange camera_to_pipeline;
  DisplayExchange pipeline_to_display;
  DisplayExchange decoder_display;  // For fake decoder output
  SerialPort serial;

  // Apply fake mode
  serial.set_fake(cfg.serial.fake);

  if (cfg.camera.fake) LOG_INFO("Camera: FAKE mode (synthetic frames)");
  if (cfg.serial.fake) LOG_INFO("Serial: FAKE mode (memory loopback)");

  // Create components
  Camera camera(cfg, camera_to_pipeline, g_running);
  Pipeline pipeline(cfg, camera_to_pipeline, pipeline_to_display, serial, g_running);

  // Start threads
  std::thread camera_thread([&camera]() { camera.run(); });
  std::thread pipeline_thread([&pipeline]() { pipeline.run(); });

  // Encoder-side display
  std::thread display_thread;
  if (cfg.display.enable) {
    display_thread = std::thread([&cfg, &pipeline_to_display]() {
      Display display(cfg, pipeline_to_display, g_running);
      display.run();
    });
  }

  // Fake decoder display (only in fake serial mode)
  std::thread decoder_thread;
  if (cfg.serial.fake) {
    decoder_thread = std::thread([&cfg, &serial, &decoder_display]() {
      FakeDecoder decoder(cfg, serial, decoder_display, g_running);
      decoder.run();
    });

    // Decoded output display window
    // We reuse the Display class with a different exchange for the decoded view
  }

  // Wait for shutdown
  LOG_INFO("System running. Press Ctrl+C to stop.");

  while (g_running.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  LOG_INFO("Shutting down...");
  g_running.store(false, std::memory_order_release);

  camera_thread.join();
  pipeline_thread.join();
  if (display_thread.joinable()) display_thread.join();
  if (decoder_thread.joinable()) decoder_thread.join();

  LOG_INFO("Doorlock Sniper stopped.");
  return 0;
}
