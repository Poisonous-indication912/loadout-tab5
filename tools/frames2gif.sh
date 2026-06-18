#!/usr/bin/env bash
# Une los frames del grabador del DEVICE (/sd/rec/fNNNNN.jpg) en un GIF (y MP4).
# 1) Copia la carpeta /sd/rec del Tab5 a tu Mac.
# 2) tools/frames2gif.sh ~/ruta/rec docs/device.gif [fps]
# Requiere ffmpeg (brew install ffmpeg).
set -e
DIR="${1:?Falta la carpeta con fNNNNN.jpg}"
OUT="${2:-device.gif}"
FPS="${3:-8}"
command -v ffmpeg >/dev/null || { echo "Falta ffmpeg:  brew install ffmpeg"; exit 1; }
mkdir -p "$(dirname "$OUT")"
PAL="$(mktemp -t pal).png"
echo "[1/3] paleta..."
ffmpeg -y -framerate "$FPS" -i "$DIR/f%05d.jpg" \
  -vf "scale=720:-1:flags=lanczos,palettegen=stats_mode=diff" "$PAL" >/dev/null 2>&1
echo "[2/3] gif..."
ffmpeg -y -framerate "$FPS" -i "$DIR/f%05d.jpg" -i "$PAL" \
  -lavfi "scale=720:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3" "$OUT" >/dev/null 2>&1
rm -f "$PAL"
echo "[3/3] mp4 (bonus)..."
ffmpeg -y -framerate "$FPS" -i "$DIR/f%05d.jpg" -c:v libx264 -pix_fmt yuv420p "${OUT%.*}.mp4" >/dev/null 2>&1 || true
echo "OK: $OUT ($(du -h "$OUT" | cut -f1))  +  ${OUT%.*}.mp4"
