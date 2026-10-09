#!/bin/sh
# Build and run the host tests of lib/textinput (needs gcc). Run from sw/.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-tests
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-I lib/textinput/include -o build-tests/test_textinput \
	tests/textinput/test_editor.c lib/textinput/src/*.c -lm
python3 tools/lmbuild/pack.py --bin build-tests/lm.bin de=lm/de.lm en=lm/en.lm
./build-tests/test_textinput
