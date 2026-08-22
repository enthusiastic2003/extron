#!/usr/bin/env bash
#
# Grab the Pi4's HDMI output from the USB capture dongle.
#
# Serial tells you what the kernel THINKS it did; this shows what actually
# reached the display. For framebuffer work that difference is the whole
# point — a wrong pitch, stride or pixel order produces a picture that is
# visibly and diagnosably wrong (skewed, torn, colour-swapped) where the
# serial log just says the mailbox call succeeded.
#
# Usage:
#   tools/capture.sh                  one frame -> captures/pi-<timestamp>.png
#   tools/capture.sh boot             one frame -> captures/boot.png (fixed name)
#   tools/capture.sh -v 10 clip       10s of video -> captures/clip.mkv
#   tools/capture.sh -l               list candidate capture devices and exit
#
set -euo pipefail

cd "$(dirname "$0")/.."
OUTDIR="captures"

RESOLUTION="${CAPTURE_RES:-1280x720}"
# Cheap MS2130-class dongles emit garbage for the first several frames while
# they lock to the incoming signal — we saw "Dequeued v4l2 buffer contains
# corrupted data" on the very first grab. Throw those away rather than
# capturing a torn frame and wondering whether the kernel drew it wrong.
WARMUP="${CAPTURE_WARMUP:-15}"

# Find the capture dongle BY NAME, never by a hardcoded /dev/videoN.
#
# Two reasons. Node numbers shift when devices are replugged or when the
# machine has other video hardware. More importantly this box also has a
# built-in webcam, and a stale node number could point the capture at the
# user's camera instead of the Pi. Matching on the device name means the
# worst case is "no device found", not "recorded the wrong thing".
#
# Each capture card also registers a second, metadata-only node that
# accepts no frames, so probe for one that actually yields video.
find_device() {
    local want='MACROSILICON|USB2 Video|Capture|HDMI'
    for sys in /sys/class/video4linux/video*; do
        [ -e "$sys/name" ] || continue
        local name node
        name=$(cat "$sys/name")
        node="/dev/$(basename "$sys")"
        # Skip anything that looks like a webcam, even if it somehow
        # matched above.
        if printf '%s' "$name" | grep -qiE 'webcam|integrated camera|facetime'; then
            continue
        fi
        if printf '%s' "$name" | grep -qiE "$want"; then
            # A real capture node reports at least one format; the
            # metadata sibling reports none.
            if ffmpeg -hide_banner -f v4l2 -list_formats all -i "$node" 2>&1 \
                 | grep -qE 'Raw|Compressed'; then
                printf '%s' "$node"
                return 0
            fi
        fi
    done
    return 1
}

list_devices() {
    echo "video4linux nodes:"
    for sys in /sys/class/video4linux/video*; do
        [ -e "$sys/name" ] || continue
        printf '  %-14s %s\n' "/dev/$(basename "$sys")" "$(cat "$sys/name")"
    done
}

VIDEO_SECS=0
if [ "${1:-}" = "-l" ]; then
    list_devices
    exit 0
fi
if [ "${1:-}" = "-v" ]; then
    VIDEO_SECS="${2:?-v needs a duration in seconds}"
    shift 2
fi

NAME="${1:-pi-$(date +%Y%m%d-%H%M%S)}"

DEV=$(find_device) || {
    echo "No HDMI capture device found." >&2
    echo >&2
    list_devices >&2
    echo >&2
    echo "Is the dongle plugged in? Set CAPTURE_DEV=/dev/videoN to override." >&2
    exit 1
}
DEV="${CAPTURE_DEV:-$DEV}"

mkdir -p "$OUTDIR"

if [ "$VIDEO_SECS" != "0" ]; then
    OUT="$OUTDIR/$NAME.mkv"
    echo "Recording ${VIDEO_SECS}s from $DEV -> $OUT"
    ffmpeg -hide_banner -loglevel error \
        -f v4l2 -input_format mjpeg -video_size "$RESOLUTION" \
        -i "$DEV" -t "$VIDEO_SECS" -c:v copy -y "$OUT"
else
    OUT="$OUTDIR/$NAME.png"
    echo "Capturing one frame from $DEV -> $OUT"
    # -update 1 is required for a single still: without it ffmpeg expects
    # a numbered sequence pattern in the filename and refuses.
    ffmpeg -hide_banner -loglevel error \
        -f v4l2 -input_format mjpeg -video_size "$RESOLUTION" \
        -i "$DEV" -vf "select=gte(n\,$WARMUP)" -frames:v 1 -update 1 -y "$OUT"
fi

ls -la "$OUT"
