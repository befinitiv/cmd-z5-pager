#!/bin/sh
# Two emulator instances exchange MeshCore adverts, a channel message and an
# acknowledged direct messages (the second along the learned route) over the emulated radio (UDP multicast on the
# loopback interface). Screenshots and logs are written to the given
# directory (default: build-sim/meshcore-test). Run from sw/.
set -e
out=${1:-build-sim/meshcore-test}
exe=$(pwd)/build-sim/zephyr/zephyr.exe
mkdir -p "$out/a" "$out/b"
for n in a b; do
	cp "tests/emul/meshcore_$n.txt" "$out/$n/script.txt"
	rm -f "$out/$n/flash.bin"
done
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
(cd "$out/a" && timeout 70 "$exe" --script=script.txt --meshcore > log.txt 2>&1) &
(cd "$out/b" && timeout 70 "$exe" --script=script.txt --meshcore > log.txt 2>&1) &
wait
ok=0
grep -q "advert from .* \"CMD-Z5" "$out/a/log.txt" && ok=$((ok + 1))
grep -q "advert from .* \"CMD-Z5" "$out/b/log.txt" && ok=$((ok + 1))
grep -q "channel 0: CMD-Z5 [0-9a-f]*: Hallo" "$out/b/log.txt" && ok=$((ok + 1))
[ "$(grep -c "text from [0-9a-f]*: Gut" "$out/a/log.txt")" -eq 2 ] && ok=$((ok + 1))
grep -q "message [0-9a-f]* delivered" "$out/b/log.txt" && ok=$((ok + 1))
if [ $ok -eq 5 ]; then
	echo "OK: adverts, channel and direct messages exchanged (screenshots in $out)"
else
	echo "FAILED ($ok/5): see $out/a/log.txt and $out/b/log.txt"
	exit 1
fi
