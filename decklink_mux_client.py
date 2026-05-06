#!/usr/bin/env python3
"""
Decklink multiplexed client - sends video/audio/subtitles to playout server.

This runs FFmpeg to decode the input, then sends raw streams to the server.

Usage:
    ./decklink_mux_client.py input.mov
    ./decklink_mux_client.py -ss 60 -t 10 input.mov
    ./decklink_mux_client.py input.mov --subtitle-file subs.srt
"""

import socket
import subprocess
import sys
import os
import struct
import argparse
import threading
import queue
import re

SOCKET_PATH = "/tmp/decklink.sock"
MAGIC = 0x444B4C4B
TYPE_VIDEO = 0
TYPE_AUDIO = 1
TYPE_SUBTITLE = 2
HEADER_SIZE = 25

def send_packet(sock, stream_type, data, pts, duration=0):
    """Send a packet with protocol header."""
    header = struct.pack('>IBIQQ',
        MAGIC,
        stream_type,
        len(data),
        int(pts),
        int(duration)
    )
    sock.sendall(header + data)

def parse_srt(srt_path, offset=0):
    """Parse SRT file and return list of (start_pts, end_pts, text) tuples.
    Times are in 90kHz timebase."""
    subtitles = []

    with open(srt_path, 'r', encoding='utf-8-sig') as f:
        content = f.read()

    # Split into subtitle blocks
    blocks = re.split(r'\n\n+', content.strip())

    for block in blocks:
        lines = block.strip().split('\n')
        if len(lines) < 3:
            continue

        # Parse timecode line: 00:00:00,000 --> 00:00:00,000
        time_match = re.match(
            r'(\d{2}):(\d{2}):(\d{2}),(\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2}),(\d{3})',
            lines[1]
        )
        if not time_match:
            continue

        def to_pts(h, m, s, ms):
            return int((int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000) * 90000)

        start_pts = to_pts(*time_match.groups()[:4]) + int(offset * 90000)
        end_pts = to_pts(*time_match.groups()[4:]) + int(offset * 90000)
        text = '\n'.join(lines[2:])

        subtitles.append((start_pts, end_pts, text))

    return subtitles

class MuxClient:
    def __init__(self, args):
        self.args = args
        self.sock = None
        self.running = True
        self.video_pts = 0
        self.audio_pts = 0
        self.pts_offset = args.pts_offset
        self.frame_size = args.width * args.height * 2  # UYVY422
        self.audio_frame_size = 1024 * args.channels * 2  # 1024 samples, 16-bit
        self.subtitles = []
        self.subtitle_index = 0
        self.lock = threading.Lock()

    def connect(self):
        """Connect to playout server."""
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.args.socket)
        sys.stderr.write(f"Connected to {self.args.socket}\n")

    def load_subtitles(self):
        """Load subtitles from file if specified."""
        if self.args.subtitle_file:
            offset = self.args.ss if self.args.ss else 0
            self.subtitles = parse_srt(self.args.subtitle_file, -offset)
            sys.stderr.write(f"Loaded {len(self.subtitles)} subtitles\n")

    def send_pending_subtitles(self, current_pts):
        """Send any subtitles that should appear at current_pts."""
        while self.subtitle_index < len(self.subtitles):
            start_pts, end_pts, text = self.subtitles[self.subtitle_index]
            adjusted_start = start_pts + self.pts_offset
            adjusted_end = end_pts + self.pts_offset

            if adjusted_start <= current_pts:
                duration = adjusted_end - adjusted_start
                data = text.encode('utf-8')
                with self.lock:
                    send_packet(self.sock, TYPE_SUBTITLE, data, adjusted_start, duration)
                sys.stderr.write(f"Sent subtitle: {text[:50]}...\n" if len(text) > 50 else f"Sent subtitle: {text}\n")
                self.subtitle_index += 1
            else:
                break

    def run_ffmpeg(self):
        """Run FFmpeg to decode input and capture raw video/audio."""
        # Build FFmpeg command
        cmd = [self.args.ffmpeg]

        if self.args.ss:
            cmd.extend(['-ss', str(self.args.ss)])

        cmd.extend(['-i', self.args.input])

        if self.args.t:
            cmd.extend(['-t', str(self.args.t)])

        # Video output to stdout
        cmd.extend([
            '-map', '0:v:0',
            '-f', 'rawvideo',
            '-pix_fmt', 'uyvy422',
            '-s', f'{self.args.width}x{self.args.height}',
            'pipe:1'
        ])

        # Audio output to stderr (fd 2) via separate process
        audio_cmd = [self.args.ffmpeg]

        if self.args.ss:
            audio_cmd.extend(['-ss', str(self.args.ss)])

        audio_cmd.extend(['-i', self.args.input])

        if self.args.t:
            audio_cmd.extend(['-t', str(self.args.t)])

        audio_cmd.extend([
            '-map', '0:a:0',
            '-f', 's16le',
            '-ar', str(self.args.sample_rate),
            '-ac', str(self.args.channels),
            'pipe:1'
        ])

        sys.stderr.write(f"Video cmd: {' '.join(cmd)}\n")
        sys.stderr.write(f"Audio cmd: {' '.join(audio_cmd)}\n")

        # Start both processes
        video_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )

        audio_proc = subprocess.Popen(
            audio_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )

        # Video reader thread
        def read_video():
            pts_increment = int(90000 / self.args.fps)
            frame_count = 0

            while self.running:
                data = video_proc.stdout.read(self.frame_size)
                if len(data) < self.frame_size:
                    break

                pts = self.video_pts + self.pts_offset
                with self.lock:
                    send_packet(self.sock, TYPE_VIDEO, data, pts)

                # Check for subtitles
                self.send_pending_subtitles(pts)

                self.video_pts += pts_increment
                frame_count += 1

                if frame_count % 100 == 0:
                    sys.stderr.write(f"Sent {frame_count} video frames\n")

            sys.stderr.write(f"Video done: {frame_count} frames\n")

        # Audio reader thread
        def read_audio():
            samples_per_packet = 1024
            bytes_per_sample = 2 * self.args.channels
            packet_size = samples_per_packet * bytes_per_sample
            pts_increment = int(90000 * samples_per_packet / self.args.sample_rate)
            packet_count = 0

            while self.running:
                data = audio_proc.stdout.read(packet_size)
                if len(data) < packet_size:
                    if len(data) > 0:
                        # Send partial final packet
                        pts = self.audio_pts + self.pts_offset
                        with self.lock:
                            send_packet(self.sock, TYPE_AUDIO, data, pts)
                    break

                pts = self.audio_pts + self.pts_offset
                with self.lock:
                    send_packet(self.sock, TYPE_AUDIO, data, pts)

                self.audio_pts += pts_increment
                packet_count += 1

            sys.stderr.write(f"Audio done: {packet_count} packets\n")

        video_thread = threading.Thread(target=read_video)
        audio_thread = threading.Thread(target=read_audio)

        video_thread.start()
        audio_thread.start()

        video_thread.join()
        audio_thread.join()

        video_proc.wait()
        audio_proc.wait()

    def run(self):
        """Main client loop."""
        try:
            self.connect()
            self.load_subtitles()
            self.run_ffmpeg()
        finally:
            if self.sock:
                self.sock.close()

def main():
    parser = argparse.ArgumentParser(description='Decklink multiplexed client')
    parser.add_argument('input', help='Input file')
    parser.add_argument('--socket', '-s', default=SOCKET_PATH, help='Unix socket path')
    parser.add_argument('--width', '-W', type=int, default=1920, help='Frame width')
    parser.add_argument('--height', '-H', type=int, default=1080, help='Frame height')
    parser.add_argument('--fps', '-r', type=float, default=25.0, help='Frame rate')
    parser.add_argument('--sample-rate', type=int, default=48000, help='Audio sample rate')
    parser.add_argument('--channels', type=int, default=2, help='Audio channels')
    parser.add_argument('--ffmpeg', default='./ffmpeg', help='Path to ffmpeg binary')
    parser.add_argument('-ss', type=float, help='Start time in seconds')
    parser.add_argument('-t', type=float, help='Duration in seconds')
    parser.add_argument('--subtitle-file', help='SRT subtitle file')
    parser.add_argument('--pts-offset', type=int, default=0,
                        help='PTS offset (for sequential playback, set to previous end PTS)')
    args = parser.parse_args()

    client = MuxClient(args)
    client.run()

if __name__ == '__main__':
    main()
