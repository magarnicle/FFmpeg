#!/usr/bin/env python3
"""
Decklink playout orchestrator - queues and plays multiple sources sequentially.

Usage:
    # Create a playlist file (one entry per line):
    # input.mov
    # input2.mov -ss 60 -t 10
    # input3.mov --subtitle-file subs.srt

    ./decklink_orchestrator.py playlist.txt --device "DeckLink SDI 4K"

Or run interactively:
    ./decklink_orchestrator.py --interactive --device "DeckLink SDI 4K"
    > input1.mov
    > input2.mov -ss 30 -t 10
    > quit
"""

import subprocess
import sys
import os
import argparse
import threading
import queue
import time
import shlex

class Orchestrator:
    def __init__(self, args):
        self.args = args
        self.job_queue = queue.Queue()
        self.running = True
        self.current_pts = 0
        self.server_proc = None
        self.fps = args.fps
        self.sample_rate = args.sample_rate

    def start_server(self):
        """Start the playout server."""
        cmd = [
            sys.executable, './decklink_mux_server.py',
            '--device', self.args.device,
            '--width', str(self.args.width),
            '--height', str(self.args.height),
            '--fps', str(self.args.fps),
            '--sample-rate', str(self.args.sample_rate),
            '--channels', str(self.args.channels),
            '--ffmpeg', self.args.ffmpeg,
            '--preroll', str(self.args.preroll)
        ]

        sys.stderr.write(f"Starting server: {' '.join(cmd)}\n")
        self.server_proc = subprocess.Popen(cmd)

        # Give server time to start
        time.sleep(2)

    def stop_server(self):
        """Stop the playout server."""
        if self.server_proc and self.server_proc.poll() is None:
            self.server_proc.terminate()
            self.server_proc.wait()

    def run_job(self, job_line):
        """Run a single encode job."""
        # Parse job line (input file + optional args)
        parts = shlex.split(job_line.strip())
        if not parts:
            return

        input_file = None
        extra_args = []

        i = 0
        while i < len(parts):
            if parts[i].startswith('-'):
                extra_args.append(parts[i])
                if i + 1 < len(parts) and not parts[i + 1].startswith('-'):
                    extra_args.append(parts[i + 1])
                    i += 1
            else:
                input_file = parts[i]
            i += 1

        if not input_file:
            sys.stderr.write(f"No input file in job: {job_line}\n")
            return

        # Build client command
        cmd = [
            sys.executable, './decklink_mux_client.py',
            input_file,
            '--width', str(self.args.width),
            '--height', str(self.args.height),
            '--fps', str(self.args.fps),
            '--sample-rate', str(self.args.sample_rate),
            '--channels', str(self.args.channels),
            '--ffmpeg', self.args.ffmpeg,
            '--pts-offset', str(self.current_pts)
        ] + extra_args

        sys.stderr.write(f"Running job: {' '.join(cmd)}\n")

        # Run client and wait
        proc = subprocess.Popen(cmd)
        proc.wait()

        # Estimate duration for PTS tracking
        # Try to get actual duration from ffprobe
        try:
            duration = self.get_duration(input_file, extra_args)
            self.current_pts += int(duration * 90000)
            sys.stderr.write(f"Job complete, next PTS: {self.current_pts}\n")
        except Exception as e:
            sys.stderr.write(f"Warning: couldn't get duration: {e}\n")
            # Estimate based on 10 minutes
            self.current_pts += int(600 * 90000)

    def get_duration(self, input_file, extra_args):
        """Get duration of input (accounting for -ss and -t)."""
        # First get total duration
        cmd = [
            'ffprobe', '-v', 'error',
            '-show_entries', 'format=duration',
            '-of', 'default=noprint_wrappers=1:nokey=1',
            input_file
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        total_duration = float(result.stdout.strip())

        # Apply -ss and -t if present
        ss = 0
        t = None
        i = 0
        while i < len(extra_args):
            if extra_args[i] == '-ss':
                ss = float(extra_args[i + 1])
                i += 2
            elif extra_args[i] == '-t':
                t = float(extra_args[i + 1])
                i += 2
            else:
                i += 1

        remaining = total_duration - ss
        if t is not None:
            return min(t, remaining)
        return remaining

    def process_queue(self):
        """Process jobs from queue."""
        while self.running:
            try:
                job = self.job_queue.get(timeout=1)
                if job is None:
                    break
                self.run_job(job)
            except queue.Empty:
                continue

    def run_interactive(self):
        """Run in interactive mode."""
        sys.stderr.write("Interactive mode. Enter jobs (input.mov [-ss N] [-t N] [--subtitle-file F]).\n")
        sys.stderr.write("Type 'quit' or 'exit' to stop.\n")

        while self.running:
            try:
                line = input("> ")
                if line.strip().lower() in ('quit', 'exit', 'q'):
                    break
                if line.strip():
                    self.job_queue.put(line)
            except EOFError:
                break
            except KeyboardInterrupt:
                break

        self.job_queue.put(None)

    def run_playlist(self, playlist_file):
        """Run jobs from playlist file."""
        with open(playlist_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    self.job_queue.put(line)

        self.job_queue.put(None)

    def run(self):
        """Main run loop."""
        self.start_server()

        try:
            # Start queue processor
            processor_thread = threading.Thread(target=self.process_queue)
            processor_thread.start()

            if self.args.interactive:
                self.run_interactive()
            elif self.args.playlist:
                self.run_playlist(self.args.playlist)
            else:
                sys.stderr.write("No playlist or interactive mode specified.\n")
                self.job_queue.put(None)

            processor_thread.join()
        finally:
            self.running = False
            self.stop_server()

def main():
    parser = argparse.ArgumentParser(description='Decklink playout orchestrator')
    parser.add_argument('playlist', nargs='?', help='Playlist file')
    parser.add_argument('--interactive', '-i', action='store_true', help='Interactive mode')
    parser.add_argument('--device', '-d', default='DeckLink SDI 4K', help='Decklink device name')
    parser.add_argument('--width', '-W', type=int, default=1920, help='Frame width')
    parser.add_argument('--height', '-H', type=int, default=1080, help='Frame height')
    parser.add_argument('--fps', '-r', type=float, default=25.0, help='Frame rate')
    parser.add_argument('--sample-rate', type=int, default=48000, help='Audio sample rate')
    parser.add_argument('--channels', type=int, default=2, help='Audio channels')
    parser.add_argument('--ffmpeg', default='./ffmpeg', help='Path to ffmpeg binary')
    parser.add_argument('--preroll', type=float, default=0.5, help='Decklink preroll in seconds')
    args = parser.parse_args()

    if not args.playlist and not args.interactive:
        parser.error("Either provide a playlist file or use --interactive")

    orchestrator = Orchestrator(args)
    orchestrator.run()

if __name__ == '__main__':
    main()
