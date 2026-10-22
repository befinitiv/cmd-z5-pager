#!/bin/sh
# Build and run the host tests of the fuel gauge (src/gauge.c, needs gcc). Run from anywhere.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-tests
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	-I src -o build-tests/test_gauge tests/gauge/test_gauge.c src/gauge.c
./build-tests/test_gauge
