#!/bin/sh
# make_icns.sh <ICON0.PNG> <work.png> <out.icns>: the bundle icon from the
# disc's ICON0.PNG (320x176): padded to a square, scaled, converted (sips).
set -e
rm -f "$2"
sips --padToHeightWidth 320 320 "$1" --out "$2" >/dev/null
chmod u+w "$2"
sips -z 512 512 "$2" >/dev/null
sips -s format icns "$2" --out "$3" >/dev/null
