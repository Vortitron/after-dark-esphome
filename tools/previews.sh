#!/bin/sh
# previews.sh - the builder page's previews: each saver as the ESP32 draws it,
# rendered by host/render into docs/previews/.
#
#   tools/previews.sh
#
# oled/<saver>.gif   128x64 1-bit, 15 s at 20 fps, at its own size (the page
#                    scales it up with crisp pixels)
# lcd/<saver>.gif    a blue 1602, 12 s at 8 fps, for the savers that have one,
#                    with dim cells (frame_rate_modulation) shown half-lit
# colour/<saver>.gif 320x240, 20 s at 20 fps, for the extras, which have no
#                    After Dark page for the builder to show instead
# thumb/<saver>.gif  the colour version small (128x96, 6 s at 10 fps), for the
#                    builder's modules to play on hover
# <kind>/<saver>.png a still of each (oled, lcd, thumb), shown at rest
#
# The clock is fixed (AD_TIME) and so is the seed, so a rerun only changes the
# files whose saver changed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out="$root/docs/previews"
make -s -C "$root/host"
render="$root/host/render"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out/oled" "$out/lcd" "$out/colour" "$out/thumb"
extras=$(python3 -c "import json; print(' '.join(k for k, v in json.load(open('$here/savers.json')).items() if v.get('extra')))")
export AD_TIME=1793000000
for saver in $("$render" 2>&1 | sed -n 's/^savers://p'); do
  rm -f "$tmp"/*.ppm
  "$render" "$saver" 128x64m 300 "$tmp" 20 6 > /dev/null
  ffmpeg -loglevel error -y -framerate 20 -i "$tmp/%04d.ppm" \
    -vf "split[a][b];[a]palettegen=max_colors=4:reserve_transparent=0[p];[b][p]paletteuse=dither=none" \
    "$out/oled/$saver.gif"
  convert "$tmp/0100.ppm" -define png:exclude-chunks=date,time "$out/oled/$saver.png"
  rm -f "$tmp"/*.ppm
  if AD_FRM=2 "$render" "$saver" lcd16x2 96 "$tmp" 8 6 > /dev/null 2>&1; then
    ffmpeg -loglevel error -y -framerate 8 -i "$tmp/%04d.ppm" \
      -vf "split[a][b];[a]palettegen=max_colors=8:reserve_transparent=0[p];[b][p]paletteuse=dither=none" \
      "$out/lcd/$saver.gif"
    convert "$tmp/0032.ppm" -define png:exclude-chunks=date,time "$out/lcd/$saver.png"
  fi
  rm -f "$tmp"/*.ppm
  "$render" "$saver" 320x240 60 "$tmp" 10 6 > /dev/null
  ffmpeg -loglevel error -y -framerate 10 -i "$tmp/%04d.ppm" \
    -vf "scale=128:96:flags=area,split[a][b];[a]palettegen=max_colors=48:reserve_transparent=0[p];[b][p]paletteuse=dither=none" \
    "$out/thumb/$saver.gif"
  convert "$tmp/0020.ppm" -filter Box -resize 128x96 -define png:exclude-chunks=date,time "$out/thumb/$saver.png"
  case " $extras " in *" $saver "*)
    rm -f "$tmp"/*.ppm
    "$render" "$saver" 320x240 400 "$tmp" 20 6 > /dev/null
    ffmpeg -loglevel error -y -framerate 20 -i "$tmp/%04d.ppm" \
      -vf "split[a][b];[a]palettegen=max_colors=16:reserve_transparent=0[p];[b][p]paletteuse=dither=none" \
      "$out/colour/$saver.gif"
  esac
  echo "$saver"
done
du -sh "$out"
