#!/bin/bash
# FFmpeg QC script equivalent to Venera Pulsar "ACCTV NEW Master" template
# Usage: ./pulsar_qc.sh <input_file>
#
# Runs all video and audio quality checks in a single ffmpeg decode pass
# using a complex filtergraph.

set -uo pipefail
set -o xtrace

INPUT="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FILE_DIR="$(dirname "$1")"
FFPROBE="${SCRIPT_DIR}/ffprobe"
FFMPEG="${SCRIPT_DIR}/ffmpeg"
REPORT_DIR="${SCRIPT_DIR}/qc_reports"
mkdir -p "$REPORT_DIR"
BASENAME=$(basename "$INPUT")
REPORT="$REPORT_DIR/${BASENAME%.*}_qc_report.txt"

echo "============================================" | tee "$REPORT"
echo "QC Report: $INPUT" | tee -a "$REPORT"
echo "Date: $(date)" | tee -a "$REPORT"
stat "$INPUT" | tee -a "$REPORT"
echo "============================================" | tee -a "$REPORT"

ERRORS=0
WARNINGS=0
CURIOSITIES=0

error() { echo "[ERROR] $1" | tee -a "$REPORT"; ((ERRORS++)); }
warn()  { echo "[WARN]  $1" | tee -a "$REPORT"; ((WARNINGS++)); }
curio()  { echo "[CURIO]  $1" | tee -a "$REPORT"; ((CURIOSITIES++)); }
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
MD5="" #$(md5sum "$INPUT" | awk '{print $1}')
info "MD5 Checksum: $MD5"

# ============================================================
# 2. VIDEO PARAMETER CHECKS (single ffprobe call)
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- VIDEO PARAMETER CHECKS ---" | tee -a "$REPORT"

# Extract all video stream info in one ffprobe call
V_INFO=$($FFPROBE -v error -select_streams v:0 \
    -show_entries stream=codec_name,width,height,r_frame_rate,pix_fmt,profile \
    -of default=nw=1:nk=0 "$INPUT")
V_CODEC=$(echo "$V_INFO" | grep "^codec_name=" | cut -d= -f2)
V_WIDTH=$(echo "$V_INFO" | grep "^width=" | cut -d= -f2)
V_HEIGHT=$(echo "$V_INFO" | grep "^height=" | cut -d= -f2)
V_FPS=$(echo "$V_INFO" | grep "^r_frame_rate=" | cut -d= -f2)
V_PIX_FMT=$(echo "$V_INFO" | grep "^pix_fmt=" | cut -d= -f2)
V_PROFILE=$(echo "$V_INFO" | grep "^profile=" | cut -d= -f2)

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
# 3. AUDIO PARAMETER CHECKS (single ffprobe call)
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- AUDIO PARAMETER CHECKS ---" | tee -a "$REPORT"

A_INFO=$($FFPROBE -v error -select_streams a:0 \
    -show_entries stream=codec_name,sample_rate,bits_per_raw_sample,bits_per_coded_sample,bits_per_sample \
    -of default=nw=1:nk=0 "$INPUT")
A_CODEC=$(echo "$A_INFO" | grep "^codec_name=" | cut -d= -f2)
A_SAMPLE_RATE=$(echo "$A_INFO" | grep "^sample_rate=" | cut -d= -f2)
A_BITS=$(echo "$A_INFO" | grep "^bits_per" | cut -d= -f2 | grep -v "N/A" | grep -v "^0$" | head -1)

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
# 4. VIDEO + AUDIO QUALITY CHECKS (single ffmpeg pass)
# ============================================================
echo "" | tee -a "$REPORT"
echo "--- QUALITY CHECKS (single-pass) ---" | tee -a "$REPORT"
info "Running all quality checks in one pass..."

# Single ffmpeg command with complex filtergraph:
#   Video: chain blackdetect -> freezedetect -> colorbarsdetect -> output
#   Video: chain solidcolordetect -> freezedetect -> colorbarsdetect -> output
#   Audio: asplit=4, one branch (silencedetect) to output, others to anullsink
#   This decodes the file only once instead of 7 separate passes.
             #solidcolordetect=d=0.1:grid=3:section_th=0.02 potentially only this is needed
QC_OUTPUT=$($FFMPEG -noprogress -nostdin -hide_banner -i "$INPUT" \
    -filter_complex "
        [0:v]solidcolordetect@batman=d=3:grid=3:section_th=0.02:pix_th=0.05:pic_th=0.99:dev_th=0.60,
             solidcolordetect@robin=d=0.1:grid=9:section_th=0.05:pix_th=0.1:pic_th=0.99:dev_th=0.55,
             freezedetect=n=0.001:d=20,
             colorbarsdetect=d=0.5[vout];
        [0:a]asplit=4[a1][a2][a3][a4];
        [a1]silencedetect=noise=-60dB:d=10[aout];
        [a2]ebur128=peak=true:framelog=verbose,anullsink;
        [a3]clipdetect=n=1000,anullsink;
        [a4]dualmonodetect=ratio=99,anullsink
    " -map "[vout]" -map "[aout]" -f null - 2>&1)



# Parse results from the combined output
echo "" | tee -a "$REPORT"
echo "--- VIDEO QUALITY ---" | tee -a "$REPORT"

get_screenshots() {
    issue_idx=0                
    IFS=$'\n\t'
    for issue in $(echo "$1" | \grep -o "start: \?[[:digit:]]\+.*end: \?[[:digit:]]\+\.[[:digit:]]\+")
    do                         
        start="$(echo "$issue" | grep -o "start: \?[[:digit:]]\+.[[:digit:]]\+" | sed -e "s/start: \?//")"
        start="$(echo "$start - 1" | bc)"
        end="$(echo "$issue" | grep -o "end: \?[[:digit:]]\+.[[:digit:]]\+" | sed -e "s/end: \?//")"
        end="$(echo "$end + 1" | bc)"
        duration=$(echo "$end - $start" | bc | xargs printf "%.2f\n")
        ss_dir="$REPORT_DIR/$2/${BASENAME%.*}"
        mkdir -p "$ss_dir"
        ffmpeg -ss $start -i "$INPUT" -r 15 -t $duration "${ss_dir}/screenshots_${issue_idx}_%03d.jpg" | tee -a "$REPORT"
        issue_idx=$(( issue_idx + 1 ))
    done                       
}
# Black frames
BLACKDETECT=$(echo "$QC_OUTPUT" | grep "batman.*solid_start:" | grep "color:black"  || true)
if [[ -n "$BLACKDETECT" ]]; then
    echo "$BLACKDETECT" | tee -a "$REPORT"
    warn "Black frames detected"
    get_screenshots "$BLACKDETECT" "black_frames"
else
    info "No black frames detected"
fi

# Solid color frames (excluding black, which blackdetect handles)
SOLIDCOLOR=$(echo "$QC_OUTPUT" | grep "robin.*solid_start:" | grep -v "color:black" || true)
if [[ -n "$SOLIDCOLOR" ]]; then
    echo "$SOLIDCOLOR" | tee -a "$REPORT"
    curio "Solid colour frames detected"
    get_screenshots "$SOLIDCOLOR" "coloured_frames"
else
    info "No solid colour frames detected"
fi

# Freeze frames
FREEZEDETECT=$(echo "$QC_OUTPUT" | grep "freezedetect" || true)
if [[ -n "$FREEZEDETECT" ]]; then
    echo "$FREEZEDETECT" | tee -a "$REPORT"
    error "Freeze frames detected"
    get_screenshots "$FREEZEDETECT"f "freeze_frames"
else
    info "No freeze frames detected"
fi

# Color bars
COLORBARS=$(echo "$QC_OUTPUT" | grep "colorbars_type" || true)
if [[ -n "$COLORBARS" ]]; then
    echo "$COLORBARS" | tee -a "$REPORT"
    error "Colour bars detected"
    get_screenshots "$COLORBARS" "colour_bars"
else
    info "No colour bars detected"
fi

echo "" | tee -a "$REPORT"
echo "--- AUDIO QUALITY ---" | tee -a "$REPORT"

# Silence/Mute
SILENCE=$(echo "$QC_OUTPUT" | grep "silence_" || true)
if [[ -n "$SILENCE" ]]; then
    echo "$SILENCE" | tee -a "$REPORT"
    warn "Silence detected"
else
    info "No silence/mute detected"
fi

# Loudness (EBU R128 / OP-59)
echo "$QC_OUTPUT" | grep -E "^\s+(I:|LRA:|Threshold:|Peak:)" | grep -v "Stream" | tail -10 | tee -a "$REPORT"

TOO_LOUD=0
TOO_QUIET=0
LOUD_ZONE=0
TP_OVER=0
INTEGRATED=$(echo "$QC_OUTPUT" | grep -E "^\s+I:" | tail -1 | grep -oP '[-0-9.]+' | head -1)
if [[ -n "$INTEGRATED" ]]; then
    TOO_LOUD=$(python3 -c "print(1 if $INTEGRATED > -23 else 0)")
    TOO_QUIET=$(python3 -c "print(1 if $INTEGRATED < -27 else 0)")
    if [[ "$TOO_LOUD" == "1" ]]; then
        warn "Integrated loudness is ${INTEGRATED} LKFS, exceeds max -23 LKFS (target -24 +/- 1)"
    elif [[ "$TOO_QUIET" == "1" ]]; then
        warn "Integrated loudness is ${INTEGRATED} LKFS, below min -27 LKFS (target -26 +/- 1)"
    else
        info "Integrated loudness: ${INTEGRATED} LKFS (OK, within -25 to -23)"
    fi
fi

# Check for loud zones (LRA high = 95th percentile of short-term loudness)
LRA_HIGH=$(echo "$QC_OUTPUT" | grep -E "^\s+LRA high:" | tail -1 | grep -oP '[-0-9.]+' | head -1)
if [[ -n "$LRA_HIGH" ]]; then
    LOUD_ZONE=$(python3 -c "print(1 if $LRA_HIGH > -23 else 0)")
    if [[ "$LOUD_ZONE" == "1" ]]; then
        warn "Audio loud zone found with max short-term loudness ${LRA_HIGH} LKFS (exceeds -23 LKFS)"
    fi
fi

TRUE_PEAK=$(echo "$QC_OUTPUT" | grep -E "^\s+Peak:" | tail -1 | grep -oP '[-0-9.]+' | head -1)
if [[ -n "$TRUE_PEAK" ]]; then
    TP_OVER=$(python3 -c "print(1 if $TRUE_PEAK > -2 else 0)")
    if [[ "$TP_OVER" == "1" ]]; then
        error "True peak is ${TRUE_PEAK} dBTP, exceeds -2 dBTP limit"
    else
        info "True peak: ${TRUE_PEAK} dBTP (OK, below -2 dBTP)"
    fi
fi

# Sustained clipping
CLIPPING=$(echo "$QC_OUTPUT" | grep -E "clip_channel|total_clip" || true)
if [[ -n "$CLIPPING" ]]; then
    echo "$CLIPPING" | tee -a "$REPORT"
    warn "Sustained clipping detected"
else
    info "No sustained clipping detected"
fi

# Dual mono (only warn if >80% of file is dual mono)
DUALMONO_WARN=$(echo "$QC_OUTPUT" | grep "dual mono percentage" | sed 's/.*dual mono/dual mono/' || true)
if [[ -n "$DUALMONO_WARN" ]]; then
    warn "$DUALMONO_WARN"
else
    info "No dual mono issue detected"
fi

# ============================================================
# LOUDNESS CORRECTION (if needed)
# ============================================================
NEEDS_CORRECTION=0
if [[ "$TOO_LOUD" == "1" || "$TOO_QUIET" == "1" || "$TP_OVER" == "1" ]]; then
    NEEDS_CORRECTION=1
fi

if [[ "$NEEDS_CORRECTION" == "1" ]]; then
    echo "" | tee -a "$REPORT"
    echo "--- LOUDNESS CORRECTION ---" | tee -a "$REPORT"
    CORRECTED="${FILE_DIR}/${BASENAME%.*}_corrected.mov"
    info "Loudness out of spec (I: ${INTEGRATED} LKFS, TP: ${TRUE_PEAK} dBTP) — correcting to OP-59 target"
    NORM_OUTPUT=$($FFMPEG -noprogress -nostdin -hide_banner -y -i "$INPUT" \
        -af loudnorm=I=-24:TP=-2:LRA=15:print_format=summary \
        -ar 48000 -c:v copy -c:a pcm_s16le "$CORRECTED" 2>&1)
    if [[ $? -eq 0 ]]; then
        # Extract loudnorm summary (Input/Output Integrated, TP, LRA, Threshold)
        NORM_SUMMARY=$(echo "$NORM_OUTPUT" | grep -E "^(Input|Output|Target)" || true)
        if [[ -n "$NORM_SUMMARY" ]]; then
            echo "$NORM_SUMMARY" | tee -a "$REPORT"
        fi
        info "Corrected file written to: $CORRECTED"
        mv "${CORRECTED}" "${1}"
        info "Corrected file replaced original file"
    else
        error "Loudness correction failed"
        rm "${CORRECTED}"
    fi
else
    info "Loudness within spec — no correction needed"
fi

# ============================================================
# SUMMARY
# ============================================================
echo "" | tee -a "$REPORT"
echo "============================================" | tee -a "$REPORT"
echo "SUMMARY: $ERRORS error(s), $WARNINGS warning(s)" | tee -a "$REPORT"
echo "============================================" | tee -a "$REPORT"
if [[ "$ERRORS" -gt 0 ]]
then
    exit 1
fi
if [[ "$WARNINGS" -gt 0 ]]
then
    exit 2
fi
