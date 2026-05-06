# Decklink Socket Playout System

This system enables seamless sequential playout of multiple video sources to Decklink SDI output. It solves the problem of concatenating multiple encoded streams without the glitches that occur at container boundaries.

## Architecture

```
Encoder 1 ─┐
Encoder 2 ──┼──▶ [Unix Socket] ──▶ Playout Server ──▶ FFmpeg ──▶ Decklink SDI
Encoder 3 ─┘
```

Each encoder decodes its source and sends raw video/audio/subtitle data over a Unix socket to the playout server, which feeds a single FFmpeg instance outputting to Decklink.

## Components

### decklink_mux_server.py
The playout server that:
- Listens on a Unix socket for client connections
- Receives multiplexed video/audio/subtitle packets
- Feeds raw streams to FFmpeg via named pipes
- Outputs to Decklink

### decklink_mux_client.py
The encoder client that:
- Decodes input files using FFmpeg
- Sends raw video (UYVY422) and audio (S16LE) to the server
- Optionally sends SRT subtitles
- Supports seek (-ss) and duration (-t) options

### decklink_orchestrator.py
The job orchestrator that:
- Manages the playout server lifecycle
- Queues multiple encode jobs
- Tracks PTS across jobs for seamless playback
- Supports playlist files or interactive mode

## Protocol

Each packet sent over the socket has a 25-byte header:

| Field | Size | Description |
|-------|------|-------------|
| Magic | 4 bytes | 0x444B4C4B ("DKLK") |
| Type | 1 byte | 0=video, 1=audio, 2=subtitle |
| Size | 4 bytes | Data size (big-endian) |
| PTS | 8 bytes | Presentation timestamp in 90kHz (big-endian) |
| Duration | 8 bytes | Duration in 90kHz, used for subtitles (big-endian) |

Followed by the raw data.

## Usage

### Quick Start with Orchestrator

```bash
# Create a playlist file
cat > playlist.txt << 'EOF'
/path/to/video1.mov
/path/to/video2.mov -ss 60 -t 30
/path/to/video3.mov --subtitle-file subs.srt
EOF

# Run the orchestrator
./decklink_orchestrator.py playlist.txt --device "DeckLink SDI 4K"
```

### Interactive Mode

```bash
./decklink_orchestrator.py --interactive --device "DeckLink SDI 4K"
> video1.mov
> video2.mov -ss 60 -t 30
> quit
```

### Manual Operation

Terminal 1 - Start server:
```bash
./decklink_mux_server.py --device "DeckLink SDI 4K" \
    --width 1920 --height 1080 --fps 25
```

Terminal 2 - Send first video:
```bash
./decklink_mux_client.py input1.mov
```

Terminal 3 - Send second video (after first completes):
```bash
# Calculate PTS offset based on first video's duration
./decklink_mux_client.py input2.mov --pts-offset 9000000
```

## Options

### Server Options

| Option | Default | Description |
|--------|---------|-------------|
| --socket, -s | /tmp/decklink.sock | Unix socket path |
| --device, -d | DeckLink SDI 4K | Decklink device name |
| --width, -W | 1920 | Frame width |
| --height, -H | 1080 | Frame height |
| --fps, -r | 25.0 | Frame rate |
| --sample-rate | 48000 | Audio sample rate |
| --channels | 2 | Audio channels |
| --ffmpeg | ./ffmpeg | Path to ffmpeg binary |
| --preroll | 0.5 | Decklink preroll in seconds |

### Client Options

| Option | Default | Description |
|--------|---------|-------------|
| --socket, -s | /tmp/decklink.sock | Unix socket path |
| --width, -W | 1920 | Frame width |
| --height, -H | 1080 | Frame height |
| --fps, -r | 25.0 | Frame rate |
| --sample-rate | 48000 | Audio sample rate |
| --channels | 2 | Audio channels |
| -ss | - | Start time in seconds |
| -t | - | Duration in seconds |
| --subtitle-file | - | SRT subtitle file |
| --pts-offset | 0 | PTS offset for sequential playback |

## FFmpeg Socket Server Integration

In addition to the Python-based solution, FFmpeg's Decklink output has been extended with built-in socket server support:

```bash
./ffmpeg -re -f lavfi -i "color=black:s=1920x1080:r=25" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo" \
    -t 999999 -c:v v210 -c:a pcm_s16le \
    -output_buffer_size 500000000 \
    -socket_path /tmp/decklink.sock -socket_listen 1 \
    -f decklink "DeckLink SDI 4K"
```

New options:
- `-socket_path <path>`: Unix socket path for external frame input
- `-socket_listen <0|1>`: Enable socket server mode

This requires `-output_buffer_size` to be set for the async output buffer.

## Shared Memory Ring Buffer (Fastest)

For the lowest latency cross-process frame transfer, use the shared memory ring buffer:

### Server (Playout Instance)

```bash
./ffmpeg -re -f lavfi -i "color=black:s=1920x1080:r=25" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo" \
    -t 999999 -c:v v210 -c:a pcm_s16le \
    -output_buffer_size 500000000 \
    -shm_name /decklink_buffer -shm_server 1 -shm_max_frames 120 \
    -f decklink "DeckLink SDI 4K"
```

### Clients (Encoder Instances)

```bash
# Encoder 1
./ffmpeg -i input1.mov -c:v v210 -c:a pcm_s16le \
    -shm_name /decklink_buffer -shm_client 1 \
    -f decklink "DeckLink SDI 4K"

# Encoder 2 (can start immediately, frames queue in shared memory)
./ffmpeg -i input2.mov -c:v v210 -c:a pcm_s16le \
    -shm_name /decklink_buffer -shm_client 1 \
    -f decklink "DeckLink SDI 4K"
```

### Shared Memory Options

| Option | Default | Description |
|--------|---------|-------------|
| -shm_name | - | POSIX shared memory name (e.g., /decklink_buffer) |
| -shm_server | 0 | Run as server (creates shm, reads frames) |
| -shm_client | 0 | Run as client (attaches to shm, writes frames) |
| -shm_max_frames | 60 | Maximum frames in ring buffer (8-240) |

### Architecture

```
Encoder 1 ─┐                              ┌─ Video Thread ─┐
Encoder 2 ──┼──▶ [Shared Memory Buffer] ──┼─ Audio Thread ──┼──▶ DeckLink SDK
Encoder 3 ─┘    (POSIX shm, ~16MB/frame)  └────────────────┘
```

The shared memory buffer uses:
- POSIX shared memory (`shm_open`)
- Process-shared pthread mutex and condition variables
- Lock-free ring buffer indices for minimal contention
- Automatic frame size calculation based on video format

### Advantages over Socket Approach

1. **Zero-copy potential**: Frames written directly to shared memory
2. **Lower latency**: No socket overhead or kernel transitions for data
3. **Better for large frames**: V210 frames are ~5MB each
4. **Multiple simultaneous writers**: Ring buffer handles concurrent access

### Limitations

1. Same machine only (shared memory is local)
2. Requires `-output_buffer_size` on server
3. All clients must use same video/audio format as server

## Subtitle Handling

Currently subtitles are received and logged. Future extensions could:
- Burn subtitles into video using FFmpeg's subtitle filter
- Pass subtitle data to teletext VBI encoding
- Encode as CEA-608/708 VANC data

## Limitations

1. All sources must have matching video/audio parameters (resolution, frame rate, sample rate)
2. The Python solution adds ~1 frame of latency for the socket transfer
3. Subtitle burn-in requires modification to the client

## Troubleshooting

**Connection refused**: Ensure the server is running before starting clients.

**Frame size mismatch**: Verify width/height/pixel format match between client and server.

**Audio sync issues**: Ensure sample rate matches between source and server settings.

**PTS jumps**: Use --pts-offset on subsequent clients, or use the orchestrator which handles this automatically.
