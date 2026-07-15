#!/usr/bin/env python3
"""Standalone serial decoder for Doorlock Sniper. No ROS2 dependency."""

import argparse
import queue
import signal
import sys
import threading
from pathlib import Path

import av
import cv2
import serial

from common import (
    CHUNK_BYTES,
    FLAG_STREAM_RESET,
    MAGIC,
    draw_overlay,
    parse_chunk,
)


class SerialDecoder:
    def __init__(self, args):
        self.port = args.port
        self.baud_rate = args.baud
        self.display_scale = max(1, args.scale)
        self.width = args.width
        self.height = args.height
        self.display_width = self.width * self.display_scale
        self.display_height = self.height * self.display_scale
        self.crosshair_offset_x = args.crosshair_x
        self.crosshair_offset_y = args.crosshair_y
        self.crosshair_width = args.crosshair_w

        self.running = threading.Event()
        self.running.set()

        self.codec = None
        self._reset_decoder(reason='startup')

        self.chunk_count = 0
        self.frame_count = 0
        self.gap_count = 0
        self.reset_count = 0
        self.last_chunk_seq = None

        self.frame_queue = queue.Queue(maxsize=3)

    def _reset_decoder(self, reason=''):
        self.codec = av.CodecContext.create('h264', 'r')
        self.codec.thread_type = 'FRAME'
        low_delay = getattr(av.codec.context.Flags, 'LOW_DELAY', None)
        if low_delay is None:
            low_delay = getattr(av.codec.context.Flags, 'low_delay', None)
        if low_delay is not None:
            self.codec.flags |= low_delay
        if reason:
            print(f'[WARN] Reset decoder ({reason})')

    def run(self):
        display_thread = threading.Thread(target=self._display_loop, daemon=True)
        display_thread.start()

        while self.running.is_set():
            try:
                ser = serial.Serial(self.port, self.baud_rate, timeout=1.0)
                print(f'[INFO] Serial connected: {self.port} @ {self.baud_rate}')
            except serial.SerialException as ex:
                print(f'[WARN] Serial open failed: {ex}, retrying...')
                if not self.running.wait(timeout=0):
                    threading.Event().wait(timeout=1.0)
                continue

            sync_buf = bytearray()
            try:
                while self.running.is_set():
                    data = ser.read(min(CHUNK_BYTES, max(1, CHUNK_BYTES - len(sync_buf))))
                    if not data:
                        continue
                    sync_buf.extend(data)

                    while len(sync_buf) >= CHUNK_BYTES:
                        # Find magic bytes
                        idx = sync_buf.find(MAGIC)
                        if idx < 0:
                            sync_buf = sync_buf[-1:]  # Keep last byte in case it's 0x44
                            break
                        if idx > 0:
                            sync_buf = sync_buf[idx:]
                        if len(sync_buf) < CHUNK_BYTES:
                            break

                        chunk = bytes(sync_buf[:CHUNK_BYTES])
                        sync_buf = sync_buf[CHUNK_BYTES:]

                        try:
                            self._handle_chunk(chunk)
                        except ValueError as ex:
                            print(f'[WARN] Bad chunk: {ex}')
            except serial.SerialException as ex:
                print(f'[WARN] Serial error: {ex}')
            finally:
                ser.close()

    def _handle_chunk(self, chunk):
        self.chunk_count += 1
        flags, chunk_seq, payload = parse_chunk(chunk)

        need_reset = (flags & FLAG_STREAM_RESET) != 0

        if self.last_chunk_seq is not None:
            expected = (self.last_chunk_seq + 1) & 0xFFFFFFFF
            if chunk_seq != expected:
                self.gap_count += 1
                need_reset = True

        if need_reset:
            self.reset_count += 1
            self._reset_decoder(reason='stream reset or gap')

        self.last_chunk_seq = chunk_seq
        if not payload:
            return

        try:
            for packet in self.codec.parse(payload):
                for frame in self.codec.decode(packet):
                    self._handle_frame(frame)
        except av.AVError:
            pass

        if self.chunk_count % 200 == 0:
            print(f'[INFO] chunks={self.chunk_count} frames={self.frame_count} '
                  f'gaps={self.gap_count} resets={self.reset_count}')

    def _handle_frame(self, frame):
        if frame is None or frame.width == 0 or frame.height == 0:
            return
        img = frame.to_ndarray(format='bgr24')
        if img is None or img.size == 0:
            return
        self.frame_count += 1
        try:
            self.frame_queue.put_nowait(img)
        except queue.Full:
            pass

    def _display_loop(self):
        cv2.namedWindow('Doorlock Decoder', cv2.WINDOW_NORMAL)
        cv2.resizeWindow('Doorlock Decoder', self.display_width, self.display_height)

        while self.running.is_set():
            try:
                img = self.frame_queue.get(timeout=0.05)
            except queue.Empty:
                continue

            if img is None:
                break

            img_disp = cv2.resize(img, (self.display_width, self.display_height),
                                  interpolation=cv2.INTER_NEAREST)
            draw_overlay(img_disp, self.crosshair_offset_x,
                         self.crosshair_offset_y, self.crosshair_width)
            cv2.imshow('Doorlock Decoder', img_disp)

            if cv2.waitKey(1) & 0xFF == ord('q'):
                self.running.clear()
                break

        cv2.destroyAllWindows()

    def stop(self):
        self.running.clear()
        try:
            self.frame_queue.put_nowait(None)
        except queue.Full:
            pass


def main():
    parser = argparse.ArgumentParser(description='Doorlock Sniper Serial Decoder')
    parser.add_argument('--port', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('--baud', type=int, default=921600, help='Baud rate')
    parser.add_argument('--width', type=int, default=300, help='Video width')
    parser.add_argument('--height', type=int, default=300, help='Video height')
    parser.add_argument('--scale', type=int, default=2, help='Display scale factor')
    parser.add_argument('--crosshair-x', type=int, default=0, help='Crosshair X offset')
    parser.add_argument('--crosshair-y', type=int, default=0, help='Crosshair Y offset')
    parser.add_argument('--crosshair-w', type=int, default=2, help='Crosshair line width')
    args = parser.parse_args()

    decoder = SerialDecoder(args)

    def sig_handler(sig, frame):
        decoder.stop()

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        decoder.run()
    except KeyboardInterrupt:
        pass
    finally:
        decoder.stop()


if __name__ == '__main__':
    main()
