#!/usr/bin/env python3
"""Standalone MQTT decoder for Doorlock Sniper. No ROS2 dependency.

Reads CustomByteBlock messages from MQTT, extracts 300-byte serial chunks,
decodes H.264 with PyAV, and displays via OpenCV.
"""

import argparse
import queue
import signal
import struct
import sys
import threading
from pathlib import Path

import av
import cv2

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print('ERROR: paho-mqtt not installed. pip install paho-mqtt', file=sys.stderr)
    sys.exit(1)

from common import (
    CHUNK_BYTES,
    FLAG_STREAM_RESET,
    draw_overlay,
    parse_chunk,
)


def _decode_varint(buf, offset):
    value = 0
    shift = 0
    while True:
        if offset >= len(buf):
            raise ValueError('truncated varint')
        byte = buf[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, offset
        shift += 7
        if shift >= 64:
            raise ValueError('varint too long')


def _extract_custom_data_field(payload):
    offset = 0
    while offset < len(payload):
        key, offset = _decode_varint(payload, offset)
        field_number = key >> 3
        wire_type = key & 0x07

        if wire_type == 0:
            _, offset = _decode_varint(payload, offset)
            continue
        if wire_type == 1:
            offset += 8
            continue
        if wire_type == 5:
            offset += 4
            continue
        if wire_type != 2:
            raise ValueError(f'unsupported wire type {wire_type}')

        size, offset = _decode_varint(payload, offset)
        end = offset + size
        if end > len(payload):
            raise ValueError('length-delimited field overrun')
        value = payload[offset:end]
        offset = end
        if field_number == 1:
            return value

    raise ValueError('bytes field #1 not found')


class MqttDecoder:
    def __init__(self, args):
        self.broker_host = args.broker
        self.broker_port = args.mqtt_port
        self.mqtt_topic = args.topic
        self.client_id = args.client_id
        self.display_scale = max(1, args.scale)
        self.width = args.width
        self.height = args.height
        self.display_width = self.width * self.display_scale
        self.display_height = self.height * self.display_scale
        self.crosshair_offset_x = args.crosshair_x
        self.crosshair_offset_y = args.crosshair_y
        self.crosshair_width = args.crosshair_w
        self.debug_dump_enable = args.dump
        self.debug_dump_every_n = max(1, args.dump_every)
        self.debug_dump_dir = Path(args.dump_dir) / 'custom_decoder'
        self.display_frame_counter = 0

        if self.debug_dump_enable:
            self.debug_dump_dir.mkdir(parents=True, exist_ok=True)

        self.running = threading.Event()
        self.running.set()

        self.codec = None
        self._reset_decoder(reason='startup')

        self.chunk_count = 0
        self.parsed_packet_count = 0
        self.frame_count = 0
        self.gap_count = 0
        self.reset_count = 0
        self.invalid_chunk_count = 0
        self.last_chunk_seq = None

        self.frame_queue = queue.Queue(maxsize=3)
        self.mqtt_client = None

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

    def _start_mqtt(self):
        try:
            self.mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=self.client_id)
        except AttributeError:
            self.mqtt_client = mqtt.Client(client_id=self.client_id)

        if hasattr(self.mqtt_client, 'reconnect_delay_set'):
            self.mqtt_client.reconnect_delay_set(min_delay=1, max_delay=5)

        self.mqtt_client.on_connect = self._on_connect
        self.mqtt_client.on_disconnect = self._on_disconnect
        self.mqtt_client.on_message = self._on_message
        self.mqtt_client.connect_async(self.broker_host, self.broker_port, keepalive=30)
        self.mqtt_client.loop_start()

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        code = getattr(reason_code, 'value', reason_code)
        if code == 0:
            client.subscribe(self.mqtt_topic, qos=0)
            print(f'[INFO] MQTT connected, subscribed to {self.mqtt_topic}')
        else:
            print(f'[ERROR] MQTT connect failed: {code}')

    def _on_disconnect(self, client, userdata, *args):
        code = 0
        if len(args) >= 2:
            code = getattr(args[1], 'value', args[1])
        elif len(args) == 1:
            code = getattr(args[0], 'value', args[0])
        print(f'[WARN] MQTT disconnected: {code}')

    def _on_message(self, client, userdata, msg):
        try:
            chunk = _extract_custom_data_field(msg.payload)
            self._handle_chunk(chunk)
        except Exception as ex:
            self.invalid_chunk_count += 1
            if self.invalid_chunk_count <= 5 or self.invalid_chunk_count % 50 == 0:
                print(f'[WARN] Invalid payload: {ex}')

    def _handle_chunk(self, chunk):
        self.chunk_count += 1
        flags, chunk_seq, payload = parse_chunk(chunk)

        need_reset = (flags & FLAG_STREAM_RESET) != 0

        if self.last_chunk_seq is not None:
            expected = (self.last_chunk_seq + 1) & 0xFFFFFFFF
            if chunk_seq != expected:
                self.gap_count += 1
                need_reset = True
                print(f'[WARN] Chunk gap: {self.last_chunk_seq} -> {chunk_seq}')

        if need_reset:
            self.reset_count += 1
            self._reset_decoder(reason='stream reset or gap')

        self.last_chunk_seq = chunk_seq
        if not payload:
            return

        try:
            parsed_packets = self.codec.parse(payload)
            self.parsed_packet_count += len(parsed_packets)
            for packet in parsed_packets:
                for frame in self.codec.decode(packet):
                    self._handle_frame(frame)
        except av.AVError:
            pass

        if self.chunk_count % 200 == 0:
            print(f'[INFO] chunks={self.chunk_count} h264={self.parsed_packet_count} '
                  f'frames={self.frame_count} gaps={self.gap_count} resets={self.reset_count}')

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

    def run(self):
        display_thread = threading.Thread(target=self._display_loop, daemon=True)
        display_thread.start()

        self._start_mqtt()
        print(f'[INFO] MQTT decoder: mqtt://{self.broker_host}:{self.broker_port}/{self.mqtt_topic}')

        # Block until shutdown
        while self.running.is_set():
            self.running.wait(timeout=1.0)

    def _display_loop(self):
        cv2.namedWindow('CustomByteBlock Decoder', cv2.WINDOW_NORMAL)
        cv2.resizeWindow('CustomByteBlock Decoder', self.display_width, self.display_height)

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
            cv2.imshow('CustomByteBlock Decoder', img_disp)

            if self.debug_dump_enable:
                self.display_frame_counter += 1
                if self.display_frame_counter % self.debug_dump_every_n == 0:
                    out = self.debug_dump_dir / f'custom_decoder_{self.display_frame_counter:08d}.png'
                    cv2.imwrite(str(out), img_disp)

            if cv2.waitKey(1) & 0xFF == ord('q'):
                self.running.clear()
                break

        cv2.destroyAllWindows()

    def stop(self):
        self.running.clear()
        if self.mqtt_client:
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()
        try:
            self.frame_queue.put_nowait(None)
        except queue.Full:
            pass


def main():
    parser = argparse.ArgumentParser(description='Doorlock Sniper MQTT Decoder')
    parser.add_argument('--broker', default='192.168.12.1', help='MQTT broker host')
    parser.add_argument('--mqtt-port', type=int, default=3333, help='MQTT broker port')
    parser.add_argument('--topic', default='CustomByteBlock', help='MQTT topic')
    parser.add_argument('--client-id', default='doorlock_decoder', help='MQTT client ID')
    parser.add_argument('--width', type=int, default=300, help='Video width')
    parser.add_argument('--height', type=int, default=300, help='Video height')
    parser.add_argument('--scale', type=int, default=2, help='Display scale')
    parser.add_argument('--crosshair-x', type=int, default=0)
    parser.add_argument('--crosshair-y', type=int, default=0)
    parser.add_argument('--crosshair-w', type=int, default=2)
    parser.add_argument('--dump', action='store_true', help='Enable debug frame dump')
    parser.add_argument('--dump-every', type=int, default=20, help='Dump every N frames')
    parser.add_argument('--dump-dir', default='sniper_debug_imgs', help='Dump directory')
    args = parser.parse_args()

    decoder = MqttDecoder(args)

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
