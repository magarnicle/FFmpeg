#!/usr/bin/env python3
"""
Simple frame server for seamless Decklink playout.

Encoders connect via Unix socket and send raw video frames.
This script forwards them to FFmpeg for Decklink output.

Usage:
    # Terminal 1: Start the frame server and playout
    ./frame_server.py | ./ffmpeg -f rawvideo -pix_fmt uyvy422 -s 1920x1080 -r 25 \
        -i pipe:0 -c:v v210 -f decklink "DeckLink SDI 4K"

    # Terminal 2+: Send encoded frames
    ./ffmpeg -i input.mov -f rawvideo -pix_fmt uyvy422 - | nc -U /tmp/decklink.sock
"""

import socket
import os
import sys
import signal
import argparse

SOCKET_PATH = "/tmp/decklink.sock"

def cleanup(sock_path):
    try:
        os.unlink(sock_path)
    except OSError:
        pass

def main():
    parser = argparse.ArgumentParser(description='Frame server for Decklink playout')
    parser.add_argument('--socket', '-s', default=SOCKET_PATH, help='Unix socket path')
    parser.add_argument('--buffer', '-b', type=int, default=65536, help='Read buffer size')
    args = parser.parse_args()

    cleanup(args.socket)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(args.socket)
    server.listen(1)

    def handle_signal(signum, frame):
        cleanup(args.socket)
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    sys.stderr.write(f"Frame server listening on {args.socket}\n")
    sys.stderr.write("Waiting for encoder connections...\n")
    sys.stderr.flush()

    stdout = os.fdopen(sys.stdout.fileno(), 'wb', buffering=0)

    total_bytes = 0
    connection_count = 0

    try:
        while True:
            conn, addr = server.accept()
            connection_count += 1
            sys.stderr.write(f"Encoder {connection_count} connected\n")
            sys.stderr.flush()

            conn_bytes = 0
            try:
                while True:
                    data = conn.recv(args.buffer)
                    if not data:
                        break
                    stdout.write(data)
                    conn_bytes += len(data)
                    total_bytes += len(data)
            except BrokenPipeError:
                sys.stderr.write("Playout pipe closed\n")
                break
            finally:
                conn.close()
                sys.stderr.write(f"Encoder {connection_count} finished ({conn_bytes:,} bytes, total: {total_bytes:,})\n")
                sys.stderr.flush()

    finally:
        cleanup(args.socket)

if __name__ == '__main__':
    main()
