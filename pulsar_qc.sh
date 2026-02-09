#!/bin/bash
# FFmpeg QC script equivalent to Venera Pulsar "ACCTV NEW Master" template
# Usage: ./pulsar_qc.sh <input_file>

set -euo pipefail

INPUT="$1"
FFPROBE="ffprobe"
FFMPEG="ffmpeg"
REPORT_DIR="./qc_reports"
mkdir -p "$REPORT_DIR"
BASENAME=$(basename "$INPUT")
REPORT="$REPORT_DIR/${BASENAME%.*}_qc_report.txt"

echo "============================================" | tee "$REPORT"
echo "QC Report: $INPUT" | tee -a "$REPORT"
echo "Date: $(date)" | tee -a "$REPORT"
echo "============================================" | tee -a "$REPORT"

ERRORS=0
WARNINGS=0

error() { echo "[ERROR] $1" | tee -a "$REPORT"; ((ERRORS++)); }
warn()  { echo "[WARN]  $1" | tee -a "$REPORT"; ((WARNINGS++)); }
info()  { echo "[INFO]  $1" | tee -a "$REPORT"; }

# ============================================================
# 1. CONTAINER CHECKS
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- CONTAINER CHECKS ---" | tee -a "$REPORT"

# File extension check
EXT="${INPUT##*.}"
EXT_LOWER=$(echo "$EXT" | tr '[:upper:]' '[:lower:]')
if [[ "$EXT_LOWER" != "mov" ]]; then
    error "File extension is .$EXT, expected .mov"
fi

# File size check (minimum 10 MB)
FILE_SIZE=$(stat -c%s "$INPUT" 2>/dev/null || stat -f%z "$INPUT" 2>/dev/null)
FILE_SIZE_MB=$((FILE_SIZE / 1048576))
if [[ $FILE_SIZE_MB -lt 10 ]]; then
    error "File size is ${FILE_SIZE_MB} MB, minimum is 10 MB"
else
    info "File size: ${FILE_SIZE_MB} MB (OK)"
fi

# Container format check
FORMAT=$($FFPROBE -v error -show_entries format=format_name -of csv=p=0 "$INPUT")
if [[ "$FORMAT" != *"mov"* && "$FORMAT" != *"mp4"* ]]; then
    error "Container format is '$FORMAT', expected MOV/MP4"
else
    info "Container format: $FORMAT (OK)"
fi

# MD5 checksum
MD5=$(md5sum "$INPUT" | awk '{print $1}')
info "MD5 Checksum: $MD5"

# ============================================================
# 2. VIDEO PARAMETER CHECKS
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- VIDEO PARAMETER CHECKS ---" | tee -a "$REPORT"

# Extract video stream info
V_CODEC=$($FFPROBE -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$INPUT")
V_WIDTH=$($FFPROBE -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$INPUT")
V_HEIGHT=$($FFPROBE -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$INPUT")
V_FPS=$($FFPROBE -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$INPUT")
V_PIX_FMT=$($FFPROBE -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$INPUT")
V_PROFILE=$($FFPROBE -v error -select_streams v:0 -show_entries stream=profile -of csv=p=0 "$INPUT")

# Codec check: ProRes
if [[ "$V_CODEC" != "prores" ]]; then
    error "Video codec is '$V_CODEC', expected 'prores'"
else
    info "Video codec: $V_CODEC (OK)"
fi

# ProRes profile check: 422 LT
if [[ "$V_PROFILE" != *"LT"* && "$V_PROFILE" != *"lt"* ]]; then
    error "ProRes profile is '$V_PROFILE', expected 'ProRes 422 LT'"
else
    info "ProRes profile: $V_PROFILE (OK)"
fi

# Chroma format check: 4:2:2
if [[ "$V_PIX_FMT" != *"422"* && "$V_PIX_FMT" != "yuv422p10le" && "$V_PIX_FMT" != "yuv422p" ]]; then
    error "Chroma format is '$V_PIX_FMT', expected 4:2:2"
else
    info "Chroma format: $V_PIX_FMT (OK)"
fi

# Resolution check
if [[ "$V_WIDTH" != "1920" ]]; then
    error "Display width is $V_WIDTH, expected 1920"
else
    info "Display width: $V_WIDTH (OK)"
fi
if [[ "$V_HEIGHT" != "1080" ]]; then
    error "Display height is $V_HEIGHT, expected 1080"
else
    info "Display height: $V_HEIGHT (OK)"
fi

# Frame rate check: 25fps (PAL)
V_FPS_EVAL=$(echo "$V_FPS" | bc -l 2>/dev/null || python3 -c "print($V_FPS)")
V_FPS_INT=$(printf "%.0f" "$V_FPS_EVAL")
if [[ "$V_FPS_INT" != "25" ]]; then
    error "Frame rate is $V_FPS ($V_FPS_EVAL), expected 25"
else
    info "Frame rate: $V_FPS ($V_FPS_EVAL fps) (OK)"
fi

# ============================================================
# 3. AUDIO PARAMETER CHECKS
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- AUDIO PARAMETER CHECKS ---" | tee -a "$REPORT"

A_CODEC=$($FFPROBE -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$INPUT")
A_SAMPLE_RATE=$($FFPROBE -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$INPUT")
A_BITS=$($FFPROBE -v error -select_streams a:0 -show_entries stream=bits_per_raw_sample,bits_per_coded_sample -of csv=p=0 "$INPUT" | tr ',' '\n' | grep -v "^$" | head -1)

# Codec check: PCM (sowt, twos, or raw)
if [[ "$A_CODEC" != "pcm_s16le" && "$A_CODEC" != "pcm_s16be" && "$A_CODEC" != "pcm_s24le" && "$A_CODEC" != "pcm_s24be" && "$A_CODEC" != *"pcm"* ]]; then
    error "Audio codec is '$A_CODEC', expected PCM (SOWT/TWOS/RAW)"
else
    info "Audio codec: $A_CODEC (OK)"
fi

# Sample rate check
if [[ "$A_SAMPLE_RATE" != "48000" ]]; then
    error "Audio sample rate is $A_SAMPLE_RATE, expected 48000"
else
    info "Audio sample rate: $A_SAMPLE_RATE (OK)"
fi

# Bit depth check
if [[ "$A_BITS" != "16" ]]; then
    error "Audio bit depth is $A_BITS, expected 16"
else
    info "Audio bit depth: $A_BITS (OK)"
fi

# ============================================================
# 4. VIDEO QUALITY CHECKS
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- VIDEO QUALITY CHECKS ---" | tee -a "$REPORT"

# Black frames detection: luma level <= 20 (8-bit scale), >75 consecutive frames = >3s at 25fps
info "Running black frame detection (luma<=20, >75 frames = >3s)..."
$FFMPEG -i "$INPUT" -vf "blackdetect=d=3:pix_th=0.0784:pic_th=0.98" -an -f null - 2>&1 \
    | grep "blackdetect" | tee -a "$REPORT" || true
# pix_th=0.0784 approximates luma level 20 on 8-bit (20/255)

# Freeze frame detection: sensitivity 1, min duration 20s
info "Running freeze frame detection (min duration 20s)..."
$FFMPEG -i "$INPUT" -vf "freezedetect=n=0.001:d=20" -an -f null - 2>&1 \
    | grep "freezedetect" | tee -a "$REPORT" || true

# ============================================================
# 5. AUDIO QUALITY CHECKS
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- AUDIO QUALITY CHECKS ---" | tee -a "$REPORT"

# Silence/Mute detection: 10 seconds
info "Running mute/silence detection (duration >= 10s)..."
$FFMPEG -i "$INPUT" -af "silencedetect=noise=-60dB:d=10" -vn -f null - 2>&1 \
    | grep "silence_" | tee -a "$REPORT" || true

# Loudness measurement (EBU R128 / OP-59 mode)
# OP-59: Target -24 LKFS, tolerance +/-1 (so -25 to -23 LKFS)
# True Peak: -2 dBTP
info "Running EBU R128 loudness measurement (OP-59: target -24 LKFS, TP -2 dBTP)..."
LOUDNESS_OUTPUT=$($FFMPEG -i "$INPUT" -af "ebur128=peak=true:framelog=verbose" -vn -f null - 2>&1)
echo "$LOUDNESS_OUTPUT" | grep -E "I:|LRA:|Threshold:|Peak:" | tail -10 | tee -a "$REPORT"

# Parse integrated loudness
INTEGRATED=$(echo "$LOUDNESS_OUTPUT" | grep "I:" | tail -1 | grep -oP '[-0-9.]+' | head -1)
if [[ -n "$INTEGRATED" ]]; then
    # Check against -24 LKFS +/- 1
    TOO_LOUD=$(echo "$INTEGRATED > -23" | bc -l 2>/dev/null || python3 -c "print(1 if $INTEGRATED > -23 else 0)")
    TOO_QUIET=$(echo "$INTEGRATED < -25" | bc -l 2>/dev/null || python3 -c "print(1 if $INTEGRATED < -25 else 0)")
    if [[ "$TOO_LOUD" == "1" ]]; then
        warn "Integrated loudness is ${INTEGRATED} LKFS, exceeds max -23 LKFS (target -24 +/- 1)"
    elif [[ "$TOO_QUIET" == "1" ]]; then
        warn "Integrated loudness is ${INTEGRATED} LKFS, below min -25 LKFS (target -24 +/- 1)"
    else
        info "Integrated loudness: ${INTEGRATED} LKFS (OK, within -25 to -23)"
    fi
fi

# Parse true peak
TRUE_PEAK=$(echo "$LOUDNESS_OUTPUT" | grep "Peak:" | tail -1 | grep -oP '[-0-9.]+' | head -1)
if [[ -n "$TRUE_PEAK" ]]; then
    TP_OVER=$(echo "$TRUE_PEAK > -2" | bc -l 2>/dev/null || python3 -c "print(1 if $TRUE_PEAK > -2 else 0)")
    if [[ "$TP_OVER" == "1" ]]; then
        error "True peak is ${TRUE_PEAK} dBTP, exceeds -2 dBTP limit"
    else
        info "True peak: ${TRUE_PEAK} dBTP (OK, below -2 dBTP)"
    fi
fi

# Audio clipping detection using astats
info "Running clipping detection..."
CLIP_OUTPUT=$($FFMPEG -i "$INPUT" -af "astats=metadata=1:reset=0" -vn -f null - 2>&1)
# astats reports number of clipped samples
CLIPPED=$(echo "$CLIP_OUTPUT" | grep -i "Number of clips" | tail -1 || true)
if [[ -n "$CLIPPED" ]]; then
    echo "$CLIPPED" | tee -a "$REPORT"
fi

# ============================================================
# 6. LOUDNESS CORRECTION (if needed)
# ============================================================
# To correct loudness to OP-59 target (-24 LKFS), uncomment:
# $FFMPEG -i "$INPUT" -af loudnorm=I=-24:TP=-2:LRA=15 -c:v copy "$REPORT_DIR/${BASENAME%.*}_corrected.mov"

# ============================================================
# SUMMARY
# ============================================================
echo "" | tee -a "$REPORT"
echo "============================================" | tee -a "$REPORT"
echo "SUMMARY: $ERRORS error(s), $WARNINGS warning(s)" | tee -a "$REPORT"
echo "============================================" | tee -a "$REPORT"
