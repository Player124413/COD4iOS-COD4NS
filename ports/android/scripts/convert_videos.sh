#!/usr/bin/env bash
# Transcode the game's Bink movies to MP4 so the Android port can play them.
#
# No Bink runtime exists for Android, so ports/android/engine/video_player_android.cpp
# decodes video/<name>.mp4 with MediaCodec instead of video/<name>.bik. This converts the
# files next to the originals with ffmpeg; the launcher's import step then copies the .mp4
# files along with the rest of the game data.
#
# Differences from the iOS script (ports/ios/scripts/convert_videos.sh):
#   * Baseline-compatible High profile at level 4.0 and an explicit keyframe interval.
#     Android hardware decoders are far less forgiving than VideoToolbox: several Mali and
#     older Adreno parts refuse streams above level 4.1 or with B-pyramids, and fall back to
#     a software decoder that cannot keep up with a cutscene while the game is also running.
#   * The audio is written to a separate .mp3 rather than muxed in. The engine plays cutscene
#     audio through its own mixer so the volume slider and subtitles stay in sync, and the
#     video decoder here is video-only.
#   * The picture is capped at 1280x720. The source movies are 720p or smaller and upscaling
#     costs bandwidth for nothing, but a few community re-releases ship 1080p files that make
#     a mid-range phone drop frames during the intro.
#
# Usage:
#   ports/android/scripts/convert_videos.sh <game-dir> [name ...]
#
# With no names, the startup and menu movies are converted (the whole set is ~2.5 GB of
# source material and takes a long time). Pass names without extension for specific movies,
# or "all" for everything.
set -euo pipefail

GAME_DIR="${1:?usage: convert_videos.sh <game-dir> [name ...]}"
shift || true
VIDEO_DIR="$GAME_DIR/main/video"
[ -d "$VIDEO_DIR" ] || { echo "No video directory at $VIDEO_DIR" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found (apt install ffmpeg / brew install ffmpeg)" >&2; exit 1; }

# Legal screens, the main-menu attract loop and the campaign intro.
DEFAULT_NAMES=(atvi iw_logo infinity_ward attract intro_movie)

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
    names=("${DEFAULT_NAMES[@]}")
elif [ "${names[0]}" = "all" ]; then
    names=()
    for file in "$VIDEO_DIR"/*.bik; do
        [ -e "$file" ] || continue
        names+=("$(basename "$file" .bik)")
    done
fi

converted=0
for name in "${names[@]}"; do
    src="$VIDEO_DIR/$name.bik"
    dst="$VIDEO_DIR/$name.mp4"
    audio="$VIDEO_DIR/$name.mp3"

    if [ ! -f "$src" ]; then
        echo "skip $name (no $name.bik)"
        continue
    fi
    if [ -f "$dst" ] && [ "$dst" -nt "$src" ]; then
        echo "have $name.mp4"
        continue
    fi

    echo "converting $name ..."
    # -bf 0 and -refs 1: no B-frames and a single reference frame. Both are the conservative
    # choice that every Android decoder handles in hardware, and the size difference on
    # source material this old is a few percent.
    # -g 60: a keyframe every second, so a seek or a restart after an interruption does not
    # have to decode from the beginning.
    nice -n 19 ffmpeg -nostdin -loglevel error -y -i "$src" \
        -an \
        -c:v libx264 -profile:v high -level:v 4.0 -preset slow -crf 21 \
        -bf 0 -refs 1 -g 60 -keyint_min 60 \
        -pix_fmt yuv420p \
        -vf "scale='min(1280,iw)':'min(720,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2" \
        -movflags +faststart "$dst"

    # Audio out separately for the engine's own mixer. Skipped silently when the source has
    # no audio track, which some of the logo stings do not.
    if ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 "$src" \
        2>/dev/null | grep -q audio; then
        nice -n 19 ffmpeg -nostdin -loglevel error -y -i "$src" \
            -vn -c:a libmp3lame -q:a 4 -ar 44100 "$audio"
    fi

    converted=$((converted + 1))
done
echo "done ($converted converted)"
