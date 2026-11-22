#!/bin/sh
# The MeshCore app's link (companion protocol) in the emulator: instance A
# offers it on TCP, the meshcore Python library drives it as the app does
# (device info, channels, contacts, a direct message with its ACK, a channel
# message, a received message); instance B is the other node
# (tests/emul/meshcore_b.txt). Then instance C against a simulated repeater
# (fake_repeater.py): logins (direct and by flood), status, telemetry, a
# command and its reply, a trace, path discovery. Needs the library and
# pycryptodome: PYTHON=<python with "pip install meshcore">. Run from sw/.
set -e
out=${1:-build-sim/app-test}
py=${PYTHON:-python3}
port=${APP_PORT:-5100}
exe=$(pwd)/build-sim/zephyr/zephyr.exe
if ! "$py" -c "import meshcore" 2>/dev/null; then
	echo "SKIPPED: no meshcore Python library (set PYTHON)"
	exit 0
fi
mkdir -p "$out/a" "$out/b"
cp tests/emul/meshcore_b.txt "$out/b/script.txt"
rm -f "$out/a/flash.bin" "$out/b/flash.bin"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
(cd "$out/a" && timeout 80 "$exe" --meshcore --app-port="$port" > log.txt 2>&1) &
(cd "$out/b" && timeout 80 "$exe" --script=script.txt --meshcore > log.txt 2>&1) &
sleep 2
status=0
if timeout 75 "$py" -u tests/emul/app_test.py "$port" "CMD-Z5" > "$out/test.txt" 2>&1; then
	echo "OK: $(tail -1 "$out/test.txt")"
else
	echo "FAILED: see $out/test.txt and $out/a/log.txt"
	status=1
fi
wait 2>/dev/null || true

# Phase 2: a repeater. The tests start after C's first advert (10-15 s), which
# would collide with an answer.
mkdir -p "$out/c"
rm -f "$out/c/flash.bin"
(cd "$out/c" && timeout 130 "$exe" --meshcore --app-port="$port" > log.txt 2>&1) &
"$py" -u tests/emul/fake_repeater.py 125 > "$out/repeater.txt" 2>&1 &
sleep 17
pub=$(head -1 "$out/repeater.txt" | cut -d' ' -f2)
if timeout 100 "$py" -u tests/emul/app_repeater_test.py "$port" "$pub" > "$out/repeater_test.txt" 2>&1; then
	echo "OK: $(tail -1 "$out/repeater_test.txt")"
else
	echo "FAILED: see $out/repeater_test.txt, $out/repeater.txt and $out/c/log.txt"
	status=1
fi
kill $(jobs -p) 2>/dev/null || true
wait 2>/dev/null || true
exit $status
