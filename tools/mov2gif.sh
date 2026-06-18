#!/usr/bin/env bash
# Convierte un video (.mov/.mp4) a un GIF optimizado para README de GitHub.
# Uso:   tools/mov2gif.sh entrada.mov [salida.gif] [fps] [ancho]
# Requiere ffmpeg:  brew install ffmpeg
#
# Flujo recomendado para grabar el simulador (sin instalar nada):
#   1) ./build/desktop/app_desktop_build   (abre el simulador)
#   2) Cmd+Shift+5 -> "Grabar parte seleccionada" -> arrastra sobre la ventana
#      del simulador -> Grabar. Para -> se guarda un .mov en el Escritorio.
#   3) tools/mov2gif.sh ~/Desktop/grabacion.mov docs/loadout.gif
set -e

IN="${1:?Falta el video de entrada (.mov/.mp4)}"
OUT="${2:-${IN%.*}.gif}"
FPS="${3:-15}"
W="${4:-720}"

command -v ffmpeg >/dev/null || { echo "ffmpeg no instalado:  brew install ffmpeg"; exit 1; }

PAL="$(mktemp -t pal).png"
echo "[1/2] paleta..."
ffmpeg -y -i "$IN" -vf "fps=$FPS,scale=$W:-1:flags=lanczos,palettegen=stats_mode=diff" "$PAL" >/dev/null 2>&1
echo "[2/2] gif -> $OUT"
ffmpeg -y -i "$IN" -i "$PAL" \
  -lavfi "fps=$FPS,scale=$W:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3" \
  "$OUT" >/dev/null 2>&1
rm -f "$PAL"
SZ=$(du -h "$OUT" | cut -f1)
echo "OK: $OUT ($SZ)"
