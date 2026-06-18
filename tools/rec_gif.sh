#!/usr/bin/env bash
# Graba la ventana del simulador (modo seleccion) y la convierte a GIF.
# Uso:   tools/rec_gif.sh [segundos] [salida.gif]
#   p.ej. tools/rec_gif.sh 20 docs/home.gif
#
# Al ejecutarlo: arrastra un recuadro SOBRE la ventana del simulador y suelta.
# Grabara N segundos (mostrando los clicks) y generara el GIF.
# Requiere ffmpeg (brew install ffmpeg). screencapture es nativo de macOS.
set -e
SEC="${1:-20}"
OUT="${2:-docs/loadout.gif}"
MOV="/tmp/loadout_rec.mov"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

command -v ffmpeg >/dev/null || { echo "Falta ffmpeg:  brew install ffmpeg"; exit 1; }
mkdir -p "$(dirname "$ROOT/$OUT")"
rm -f "$MOV"

echo ">> Arrastra un recuadro sobre la ventana del SIMULADOR y suelta."
echo ">> Grabacion de ${SEC}s (con clicks). Muevete por la UI."
screencapture -v -V"$SEC" -k "$MOV"

[ -f "$MOV" ] || { echo "No se grabo nada (cancelado?)."; exit 1; }
echo ">> Convirtiendo a GIF..."
"$ROOT/tools/mov2gif.sh" "$MOV" "$ROOT/$OUT" 15 720
echo ">> Listo: $OUT"
