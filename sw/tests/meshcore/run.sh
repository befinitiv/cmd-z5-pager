#!/bin/sh
# Build and run the host tests of lib/meshcore (needs gcc). Run from anywhere.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-tests
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-I lib/meshcore/include -I lib/meshtastic/include -I tests/meshcore \
	-o build-tests/test_meshcore tests/meshcore/*.c lib/meshcore/src/*.c \
	lib/meshtastic/src/aes.c lib/meshtastic/src/sha256.c lib/meshtastic/src/x25519.c
./build-tests/test_meshcore
