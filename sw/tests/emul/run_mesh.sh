#!/bin/sh
# Two emulator instances exchange Meshtastic messages, positions and the time over the
# emulated radio
# (UDP multicast on the loopback interface). Screenshots and logs are written
# to the given directory (default: build-sim/mesh-test). Run from sw/.
set -e
out=${1:-build-sim/mesh-test}
exe=$(pwd)/build-sim/zephyr/zephyr.exe
mkdir -p "$out/a" "$out/b"
for n in a b; do
	cp "tests/emul/mesh_$n.txt" "$out/$n/script.txt"
	rm -f "$out/$n/flash.bin"
done
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
# About 11 km apart: both broadcast their (approximate) position after the
# first fix, which comes after the message exchange and at different times
# (positions are not acknowledged, so a collision would lose them).
(cd "$out/a" && timeout 70 "$exe" --script=script.txt --node-num=aaaa0001 \
	--gps=52.5200,13.4050,34 --gps-ttff=22 > log.txt 2>&1) &
# B starts without a clock, like the board, and takes the time from A's
# position (before its own GPS fix).
(cd "$out/b" && timeout 70 "$exe" --script=script.txt --node-num=bbbb0002 --no-clock \
	--gps=52.6000,13.5000,50 --gps-ttff=26 > log.txt 2>&1) &
wait
ok=0
grep -q "text from bbbb0002: Gut" "$out/a/log.txt" && ok=$((ok + 1))
grep -q "text from aaaa0001: Hallo" "$out/b/log.txt" && ok=$((ok + 1))
grep -q "position from bbbb0002" "$out/a/log.txt" && ok=$((ok + 1))
grep -q "position from aaaa0001" "$out/b/log.txt" && ok=$((ok + 1))
grep -q "clock set from aaaa0001" "$out/b/log.txt" && ok=$((ok + 1))
if [ $ok -eq 5 ]; then
	echo "OK: messages, positions and the time exchanged (screenshots in $out)"
else
	echo "FAILED: see $out/a/log.txt and $out/b/log.txt"
	exit 1
fi
