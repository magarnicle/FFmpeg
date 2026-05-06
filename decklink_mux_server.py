#!/usr/bin/env python3
"""
Decklink multiplexed playout server - receives video/audio/subtitles via socket.

Protocol (each packet):
  - 4 bytes: magic (0x444B4C4B "DKLK")
  - 1 byte:  stream type (0=video, 1=audio, 2=subtitle)
  - 4 bytes: data size (big-endian)
  - 8 bytes: pts in timebase units (big-endian)
  - 8 bytes: duration in timebase units (big-endian, used for subtitles)
  - N bytes: data

Usage:
    # Start server
    ./decklink_mux_server.py --device "DeckLink SDI 4K"

    # Encoder sends via decklink_mux_client.py
    ./decklink_mux_client.py input.mov

Or for burned-in subtitles (simpler):
    ./decklink_mux_server.py --device "DeckLink SDI 4K" --burn-subs
"""

import socket
import subprocess
import sys
import signal
import os
import struct
import argparse
import threading
import queue
import tempfile

SOCKET_PATH = "/tmp/decklink.sock"
MAGIC = 0x444B4C4B
TYPE_VIDEO = 0
TYPE_AUDIO = 1
TYPE_SUBTITLE = 2
HEADER_SIZE = 25  # 4 + 1 + 4 + 8 + 8

def read_exact(conn, n):
    """Read exactly n bytes from socket."""
    data = b''
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def cleanup(sock_path):
    try:
        os.unlink(sock_path)
    except OSError:
        pass

class PlayoutServer:
    def __init__(self, args):
        self.args = args
        self.video_queue = queue.Queue(maxsize=100)
        self.audio_queue = queue.Queue(maxsize=200)
        self.subtitle_queue = queue.Queue(maxsize=50)
        self.running = True
        self.ffmpeg = None
        self.connection_count = 0

    def start_ffmpeg(self):
        """Start FFmpeg process for Decklink output."""
        # Create named pipes for video and audio
        self.video_pipe = f"/tmp/decklink_video_{os.getpid()}.pipe"
        self.audio_pipe = f"/tmp/decklink_audio_{os.getpid()}.pipe"

        for pipe in [self.video_pipe, self.audio_pipe]:
            if os.path.exists(pipe):
                os.unlink(pipe)
            os.mkfifo(pipe)

        # Build FFmpeg command
        cmd = [
            self.args.ffmpeg,
            '-y',
            # Video input
            '-f', 'rawvideo',
            '-pix_fmt', 'uyvy422',
            '-s', f'{self.args.width}x{self.args.height}',
            '-r', str(self.args.fps),
            '-i', self.video_pipe,
            # Audio input
            '-f', 's16le',
            '-ar', str(self.args.sample_rate),
            '-ac', str(self.args.channels),
            '-i', self.audio_pipe,
            # Output
            '-c:v', 'v210',
            '-c:a', 'pcm_s16le',
            '-preroll', str(self.args.preroll),
            '-f', 'decklink', self.args.device
        ]

        sys.stderr.write(f"Starting FFmpeg: {' '.join(cmd)}\n")

        self.ffmpeg = subprocess.Popen(
            cmd,
            stderr=subprocess.PIPE
        )

        # Thread to print FFmpeg stderr
        def print_stderr():
            for line in self.ffmpeg.stderr:
                sys.stderr.write(f"[ffmpeg] {line.decode('utf-8', errors='replace')}")
        threading.Thread(target=print_stderr, daemon=True).start()

        # Open pipes for writing
        self.video_fd = os.open(self.video_pipe, os.O_WRONLY)
        self.audio_fd = os.open(self.audio_pipe, os.O_WRONLY)

    def stop_ffmpeg(self):
        """Stop FFmpeg and cleanup pipes."""
        self.running = False

        try:
            os.close(self.video_fd)
            os.close(self.audio_fd)
        except:
            pass

        if self.ffmpeg and self.ffmpeg.poll() is None:
            self.ffmpeg.terminate()
            self.ffmpeg.wait()

        for pipe in [self.video_pipe, self.audio_pipe]:
            try:
                os.unlink(pipe)
            except:
                pass

    def video_writer_thread(self):
        """Thread that writes video frames to FFmpeg."""
        frame_size = self.args.width * self.args.height * 2  # UYVY422

        while self.running:
            try:
                data = self.video_queue.get(timeout=0.1)
                if data is None:
                    break
                if len(data) == frame_size:
                    os.write(self.video_fd, data)
                else:
                    sys.stderr.write(f"Video frame size mismatch: {len(data)} != {frame_size}\n")
            except queue.Empty:
                continue
            except BrokenPipeError:
                sys.stderr.write("Video pipe closed\n")
                break

    def audio_writer_thread(self):
        """Thread that writes audio to FFmpeg."""
        while self.running:
            try:
                data = self.audio_queue.get(timeout=0.1)
                if data is None:
                    break
                os.write(self.audio_fd, data)
            except queue.Empty:
                continue
            except BrokenPipeError:
                sys.stderr.write("Audio pipe closed\n")
                break

    def handle_client(self, conn):
        """Handle a single client connection."""
        self.connection_count += 1
        client_id = self.connection_count
        sys.stderr.write(f"Client {client_id} connected\n")

        video_frames = 0
        audio_packets = 0
        subtitle_packets = 0

        try:
            while self.running:
                # Read header
                header = read_exact(conn, HEADER_SIZE)
                if header is None:
                    break

                magic, stream_type, data_size, pts, duration = struct.unpack('>IBIQQ', header)

                if magic != MAGIC:
                    sys.stderr.write(f"Invalid magic: 0x{magic:08X}\n")
                    break

                # Read data
                data = read_exact(conn, data_size)
                if data is None:
                    break

                # Queue data for appropriate stream
                if stream_type == TYPE_VIDEO:
                    try:
                        self.video_queue.put(data, timeout=1.0)
                        video_frames += 1
                    except queue.Full:
                        sys.stderr.write("Video queue full, dropping frame\n")
                elif stream_type == TYPE_AUDIO:
                    try:
                        self.audio_queue.put(data, timeout=1.0)
                        audio_packets += 1
                    except queue.Full:
                        sys.stderr.write("Audio queue full, dropping packet\n")
                elif stream_type == TYPE_SUBTITLE:
                    # For now, log subtitles (could burn in or pass to teletext)
                    try:
                        text = data.decode('utf-8', errors='replace')
                        sys.stderr.write(f"[SUB pts={pts} dur={duration}] {text}\n")
                        subtitle_packets += 1
                    except:
                        pass

        finally:
            conn.close()
            sys.stderr.write(f"Client {client_id} disconnected: {video_frames} video, {audio_packets} audio, {subtitle_packets} subtitle packets\n")

    def run(self):
        """Main server loop."""
        cleanup(self.args.socket)

        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(self.args.socket)
        server.listen(5)
        server.settimeout(1.0)

        def handle_signal(signum, frame):
            self.running = False
        signal.signal(signal.SIGINT, handle_signal)
        signal.signal(signal.SIGTERM, handle_signal)

        sys.stderr.write(f"Listening on {self.args.socket}\n")

        # Start FFmpeg and writer threads
        self.start_ffmpeg()

        video_thread = threading.Thread(target=self.video_writer_thread, daemon=True)
        audio_thread = threading.Thread(target=self.audio_writer_thread, daemon=True)
        video_thread.start()
        audio_thread.start()

        try:
            while self.running:
                try:
                    conn, addr = server.accept()
                    # Handle client in a thread so multiple can queue
                    threading.Thread(target=self.handle_client, args=(conn,), daemon=True).start()
                except socket.timeout:
                    continue
        finally:
            self.running = False
            self.video_queue.put(None)
            self.audio_queue.put(None)
            video_thread.join(timeout=2)
            audio_thread.join(timeout=2)
            self.stop_ffmpeg()
            cleanup(self.args.socket)

def main():
    parser = argparse.ArgumentParser(description='Decklink multiplexed playout server')
    parser.add_argument('--socket', '-s', default=SOCKET_PATH, help='Unix socket path')
    parser.add_argument('--device', '-d', default='DeckLink SDI 4K', help='Decklink device name')
    parser.add_argument('--width', '-W', type=int, default=1920, help='Frame width')
    parser.add_argument('--height', '-H', type=int, default=1080, help='Frame height')
    parser.add_argument('--fps', '-r', type=float, default=25.0, help='Frame rate')
    parser.add_argument('--sample-rate', type=int, default=48000, help='Audio sample rate')
    parser.add_argument('--channels', type=int, default=2, help='Audio channels')
    parser.add_argument('--ffmpeg', default='./ffmpeg', help='Path to ffmpeg binary')
    parser.add_argument('--preroll', type=float, default=0.5, help='Decklink preroll in seconds')
    parser.add_argument('--burn-subs', action='store_true', help='Burn subtitles into video')
    args = parser.parse_args()

    server = PlayoutServer(args)
    server.run()

if __name__ == '__main__':
    main()
