#!/usr/bin/env python3
"""
Send raw video frames to the Decklink socket server.

Usage:
    ./ffmpeg -i input.mov -f rawvideo -pix_fmt uyvy422 - | ./decklink_sender.py

Or for v210:
    ./ffmpeg -i input.mov -c:v v210 -f rawvideo - | ./decklink_sender.py --format v210
"""

import socket
import struct
import sys
import argparse

SOCKET_PATH = "/tmp/decklink.sock"
MAGIC = 0x444B4C4B  # "DKLK"
TYPE_VIDEO = 0
TYPE_AUDIO = 1

def send_frame(sock, stream_type, data, pts, width=1920, height=1080):
    """Send a frame with the decklink socket protocol header."""
    header = struct.pack('>IBIQII',
        MAGIC,           # 4 bytes: magic (network order)
        stream_type,     # 1 byte: stream type
        len(data),       # 4 bytes: data size (network order)
        pts,             # 8 bytes: pts (network order)
        width,           # 4 bytes: width (network order)
        height           # 4 bytes: height (network order)
    )
    sock.sendall(header + data)

def main():
    parser = argparse.ArgumentParser(description='Send frames to Decklink socket server')
    parser.add_argument('--socket', '-s', default=SOCKET_PATH, help='Unix socket path')
    parser.add_argument('--width', '-W', type=int, default=1920, help='Frame width')
    parser.add_argument('--height', '-H', type=int, default=1080, help='Frame height')
    parser.add_argument('--fps', type=float, default=25.0, help='Frame rate')
    parser.add_argument('--format', '-f', default='uyvy422', choices=['uyvy422', 'v210'],
                        help='Pixel format')
    args = parser.parse_args()

    # Calculate frame size based on format
    if args.format == 'uyvy422':
        frame_size = args.width * args.height * 2  # 2 bytes per pixel
    elif args.format == 'v210':
        # v210: 6 pixels packed into 16 bytes (128 bits)
        pixels_per_block = 6
        bytes_per_block = 16
        blocks_per_row = (args.width + pixels_per_block - 1) // pixels_per_block
        # Round up to 128-byte alignment per row
        row_bytes = ((blocks_per_row * bytes_per_block + 127) // 128) * 128
        frame_size = row_bytes * args.height

    sys.stderr.write(f"Connecting to {args.socket}...\n")
    sys.stderr.write(f"Format: {args.format}, frame size: {frame_size} bytes\n")

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(args.socket)

    sys.stderr.write("Connected, sending frames...\n")

    pts = 0
    pts_increment = int(90000 / args.fps)  # PTS in 90kHz timebase
    frame_count = 0
    stdin = sys.stdin.buffer

    try:
        while True:
            data = stdin.read(frame_size)
            if len(data) < frame_size:
                if len(data) > 0:
                    sys.stderr.write(f"Short read: {len(data)}/{frame_size}, ending\n")
                break

            send_frame(sock, TYPE_VIDEO, data, pts, args.width, args.height)
            pts += pts_increment
            frame_count += 1

            if frame_count % 100 == 0:
                sys.stderr.write(f"Sent {frame_count} frames\n")

    except BrokenPipeError:
        sys.stderr.write("Server closed connection\n")
    finally:
        sock.close()

    sys.stderr.write(f"Done, sent {frame_count} frames\n")

if __name__ == '__main__':
    main()
