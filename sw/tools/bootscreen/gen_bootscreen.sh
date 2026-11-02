#!/bin/sh
# Builds and runs the bootloader screen generator on the host. Run from sw/:
#   tools/bootscreen/gen_bootscreen.sh > boot/src/bootscreen.h
set -e
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -O2 -Wall -Ilib/gfx/include -o "$out/gen" tools/bootscreen/gen_bootscreen.c \
	lib/gfx/src/gfx.c lib/gfx/src/fonts.c
"$out/gen"
