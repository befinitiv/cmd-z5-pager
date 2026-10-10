#!/bin/sh
# Build and run the host tests of lib/synth (needs gcc). Run from anywhere.
# Also writes build-tests/synth_demo.wav with the timbres.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-tests
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	-I lib/synth/include -o build-tests/test_synth \
	tests/synth/test_synth.c lib/synth/src/synth.c -lm
./build-tests/test_synth
