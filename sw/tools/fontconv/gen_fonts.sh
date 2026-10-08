#!/bin/sh
# Regenerate the font headers in lib/gfx/fonts from the X11 misc-misc BDF fonts
# (public domain). The BDF files are downloaded into a temporary directory.
set -e

here=$(cd "$(dirname "$0")" && pwd)
out="$here/../../lib/gfx/fonts"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

url=https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/master
for f in 4x6 5x7 10x20; do
	curl -sfL -o "$tmp/$f.bdf" "$url/$f.bdf"
done

python3 "$here/bdf2c.py" "$tmp/4x6.bdf" "$out/font_small.h" --name small
python3 "$here/bdf2c.py" "$tmp/5x7.bdf" "$out/font_text.h" --name text --prop --space 3
python3 "$here/bdf2c.py" "$tmp/10x20.bdf" "$out/font_large.h" --name large \
	--chars " 0123456789:.-+%/?!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
