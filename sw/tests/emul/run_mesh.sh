#!/bin/sh
# Two emulator instances exchange Meshtastic messages over the emulated radio
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
(cd "$out/a" && timeout 70 "$exe" --script=script.txt --node-num=aaaa0001 > log.txt 2>&1) &
(cd "$out/b" && timeout 70 "$exe" --script=script.txt --node-num=bbbb0002 > log.txt 2>&1) &
wait
ok=0
grep -q "text from bbbb0002: Gut" "$out/a/log.txt" && ok=$((ok + 1))
grep -q "text from aaaa0001: Hallo" "$out/b/log.txt" && ok=$((ok + 1))
if [ $ok -eq 2 ]; then
	echo "OK: messages exchanged (screenshots in $out)"
else
	echo "FAILED: see $out/a/log.txt and $out/b/log.txt"
	exit 1
fi
