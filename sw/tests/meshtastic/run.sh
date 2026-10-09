#!/bin/sh
# Build and run the host tests of lib/meshtastic (needs gcc). Run from anywhere.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-tests
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-I lib/meshtastic/include -I tests/meshtastic -o build-tests/test_meshtastic \
	tests/meshtastic/*.c lib/meshtastic/src/*.c -lm
./build-tests/test_meshtastic
