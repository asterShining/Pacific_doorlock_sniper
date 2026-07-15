"""Shared utilities for doorlock sniper decoders."""

import struct


CHUNK_BYTES = 300
HEADER_BYTES = 10
PAYLOAD_MAX_BYTES = 288
FLAG_STREAM_RESET = 0x01
MAGIC = bytes([0x44, 0x4C])


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if (crc & 0x0001) != 0:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc & 0xFFFF


def parse_chunk(chunk: bytes):
    """Parse a 300-byte serial chunk. Returns (flags, chunk_seq, payload) or raises ValueError."""
    if len(chunk) != CHUNK_BYTES:
        raise ValueError(f'expected {CHUNK_BYTES} bytes, got {len(chunk)}')
    if chunk[0] != 0x44 or chunk[1] != 0x4C or chunk[2] != 0x01:
        raise ValueError('invalid header magic/version')

    payload_len = chunk[8] | (chunk[9] << 8)
    if payload_len > PAYLOAD_MAX_BYTES:
        raise ValueError(f'invalid payload_len={payload_len}')

    crc_expected = chunk[298] | (chunk[299] << 8)
    crc_actual = crc16_ccitt(chunk[:298])
    if crc_expected != crc_actual:
        raise ValueError(f'crc mismatch: expected=0x{crc_expected:04x} actual=0x{crc_actual:04x}')

    flags = chunk[3]
    chunk_seq = struct.unpack_from('<I', chunk, 4)[0]
    payload = chunk[HEADER_BYTES:HEADER_BYTES + payload_len]
    return flags, chunk_seq, payload


def draw_overlay(img, crosshair_offset_x=0, crosshair_offset_y=0, crosshair_width=2):
    """Draw crosshair and center circle on display image."""
    import cv2

    h, w = img.shape[:2]
    cx = max(0, min(w - 1, w // 2 + crosshair_offset_x))
    cy = max(0, min(h - 1, h // 2 + crosshair_offset_y))

    crosshair_color = (230, 190, 235)
    cv2.line(img, (0, cy), (w - 1, cy), crosshair_color, crosshair_width, cv2.LINE_AA)
    cv2.line(img, (cx, 0), (cx, h - 1), crosshair_color, crosshair_width, cv2.LINE_AA)

    center_color = (170, 255, 170)
    cv2.circle(img, (w // 2, h // 2), 24, center_color, 1, cv2.LINE_AA)
