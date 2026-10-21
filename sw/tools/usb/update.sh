#!/bin/sh
# Firmware update over USB through MCUboot serial recovery (README, "Updates
# over USB"). Needs smpmgr (`pipx install smpmgr`; or set SMPMGR).
#
#   tools/usb/update.sh [image]    default: build-board/sw/zephyr/zephyr.signed.bin
#
# A running application is switched to the bootloader with the 1200-baud
# touch. If the application does not run, hold the power key for 2 s after
# switching on, or use Settings -> Firmware update.
set -e
img=${1:-build-board/sw/zephyr/zephyr.signed.bin}
smpmgr=${SMPMGR:-smpmgr}
ids=/dev/serial/by-id

port() {
	# $1 = bootloader | app
	for p in "$ids"/usb-befi_CMD-Z5_*-if00; do
		[ -e "$p" ] || continue
		case "$p" in
		*_bootloader_*) [ "$1" = bootloader ] && echo "$p" && return ;;
		*) [ "$1" = app ] && echo "$p" && return ;;
		esac
	done
	return 0
}

wait_port() {
	# $1 = bootloader | app, $2 = seconds
	n=$(($2 * 10))
	while [ $n -gt 0 ]; do
		p=$(port "$1")
		[ -n "$p" ] && echo "$p" && return
		sleep 0.1
		n=$((n - 1))
	done
	echo "no $1 port after $2 s" >&2
	exit 1
}

[ -f "$img" ] || { echo "image not found: $img" >&2; exit 1; }

boot=$(port bootloader)
if [ -z "$boot" ]; then
	app=$(port app)
	[ -n "$app" ] || { echo "CMD-Z5 not found on USB" >&2; exit 1; }
	echo "switching to the bootloader ($app)"
	stty -F "$app" 1200
	boot=$(wait_port bootloader 10)
fi

echo "uploading $img to $boot"
"$smpmgr" --port "$boot" image upload "$img"
"$smpmgr" --port "$boot" os reset > /dev/null
app=$(wait_port app 15)
echo "done, application running: $app"
