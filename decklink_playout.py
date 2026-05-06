#!/usr/bin/env python3
"""
Decklink playout server - receives frames via Unix socket and plays to Decklink.

This script:
1. Listens for client connections on a Unix socket
2. Receives raw video frames from encoders
3. Pipes them to FFmpeg for Decklink output

Usage:
    ./decklink_playout.py --device "DeckLink SDI 4K"

Then from encoders:
    ./ffmpeg -i input.mov -f rawvideo -pix_fmt uyvy422 - | nc -U /tmp/decklink.sock

Or for v210:
    ./ffmpeg -i input.mov -c:v v210 -f rawvideo - | nc -U /tmp/decklink.sock
"""

import socket
import subprocess
import sys
import signal
import os
import argparse

SOCKET_PATH = "/tmp/decklink.sock"

def cleanup(sock_path):
    try:
        os.unlink(sock_path)
    except OSError:
        pass

def main():
    parser = argparse.ArgumentParser(description='Decklink playout server')
    parser.add_argument('--socket', '-s', default=SOCKET_PATH, help='Unix socket path')
    parser.add_argument('--device', '-d', default='DeckLink SDI 4K', help='Decklink device name')
    parser.add_argument('--width', '-W', type=int, default=1920, help='Frame width')
    parser.add_argument('--height', '-H', type=int, default=1080, help='Frame height')
    parser.add_argument('--fps', '-r', type=float, default=25.0, help='Frame rate')
    parser.add_argument('--format', '-f', default='uyvy422', choices=['uyvy422', 'v210'],
                        help='Input pixel format from encoders')
    parser.add_argument('--ffmpeg', default='./ffmpeg', help='Path to ffmpeg binary')
    parser.add_argument('--preroll', type=float, default=0.5, help='Decklink preroll in seconds')
    args = parser.parse_args()

    cleanup(args.socket)

    # Create socket
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(args.socket)
    server.listen(5)

    def handle_signal(signum, frame):
        cleanup(args.socket)
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    # Build ffmpeg command for Decklink output
    if args.format == 'v210':
        input_codec = ['-c:v', 'v210']
        pix_fmt = []
    else:
        input_codec = []
        pix_fmt = ['-pix_fmt', args.format]

    ffmpeg_cmd = [
        args.ffmpeg,
        '-f', 'rawvideo',
        *pix_fmt,
        *input_codec,
        '-s', f'{args.width}x{args.height}',
        '-r', str(args.fps),
        '-i', 'pipe:0',
        '-c:v', 'v210',
        '-preroll', str(args.preroll),
        '-f', 'decklink', args.device
    ]

    sys.stderr.write(f"Starting Decklink playout: {' '.join(ffmpeg_cmd)}\n")
    sys.stderr.write(f"Listening on {args.socket}\n")

    # Start FFmpeg
    ffmpeg = subprocess.Popen(
        ffmpeg_cmd,
        stdin=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    # Thread to print FFmpeg stderr
    import threading
    def print_ffmpeg_stderr():
        for line in ffmpeg.stderr:
            sys.stderr.write(f"[ffmpeg] {line.decode('utf-8', errors='replace')}")
    stderr_thread = threading.Thread(target=print_ffmpeg_stderr, daemon=True)
    stderr_thread.start()

    connection_count = 0
    total_bytes = 0

    try:
        while True:
            sys.stderr.write("Waiting for encoder connection...\n")
            conn, addr = server.accept()
            connection_count += 1
            sys.stderr.write(f"Encoder {connection_count} connected\n")

            conn_bytes = 0
            try:
                while True:
                    data = conn.recv(65536)
                    if not data:
                        break
                    ffmpeg.stdin.write(data)
                    ffmpeg.stdin.flush()
                    conn_bytes += len(data)
                    total_bytes += len(data)
            except BrokenPipeError:
                sys.stderr.write("FFmpeg pipe closed\n")
                break
            finally:
                conn.close()
                sys.stderr.write(f"Encoder {connection_count} finished ({conn_bytes:,} bytes)\n")

    except KeyboardInterrupt:
        pass
    finally:
        cleanup(args.socket)
        if ffmpeg.poll() is None:
            ffmpeg.stdin.close()
            ffmpeg.wait()

    sys.stderr.write(f"Total: {total_bytes:,} bytes from {connection_count} connections\n")

if __name__ == '__main__':
    main()
