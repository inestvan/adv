#!/bin/sh
# USB UVC gadget for the SPRUT-M grabber (Raspberry Pi 4, USB-C port in device mode via dwc2).
# One uncompressed YUY2 format, one frame size: 720x576 @ 25 (PAL) or 720x480 @ 29.97 (NTSC).
# Usage: sprutm-gadget.sh [start|stop]     Standard: STD=pal|ntsc (default from /etc/default/sprutm-grabber)
set -e

[ -f /etc/default/sprutm-grabber ] && . /etc/default/sprutm-grabber
STD=${STD:-pal}
CFS=/sys/kernel/config
G=$CFS/usb_gadget/sprutm
F=$G/functions/uvc.0

stop() {
	[ -d "$G" ] || return 0
	echo "" > "$G/UDC" 2>/dev/null || true
	rm -f "$G/configs/c.1/uvc.0"
	rm -f "$F/control/class/fs/h" "$F/control/class/ss/h"
	rm -f "$F/streaming/class/fs/h" "$F/streaming/class/hs/h" "$F/streaming/class/ss/h"
	rm -f "$F/streaming/header/h/u"
	rmdir "$F/streaming/header/h" 2>/dev/null || true
	for d in "$F"/streaming/uncompressed/u/*/; do
		if [ -d "$d" ]; then rmdir "$d"; fi
	done
	rmdir "$F/streaming/uncompressed/u" 2>/dev/null || true
	rmdir "$F/control/header/h" 2>/dev/null || true
	rmdir "$F" 2>/dev/null || true
	rmdir "$G/configs/c.1/strings/0x409" "$G/configs/c.1" 2>/dev/null || true
	rmdir "$G/strings/0x409" "$G" 2>/dev/null || true
}

start() {
	modprobe libcomposite
	mountpoint -q $CFS || mount -t configfs none $CFS
	stop

	if [ "$STD" = ntsc ]; then
		W=720; H=480; IVAL=333667
	else
		W=720; H=576; IVAL=400000
	fi
	BUF=$((W * H * 2))
	RATE=$((W * H * 16 * 10000000 / IVAL))	# bits per second

	mkdir -p "$G"
	echo 0x1d6b > "$G/idVendor"		# Linux Foundation
	echo 0x0104 > "$G/idProduct"		# Multifunction Composite Gadget
	echo 0x0100 > "$G/bcdDevice"
	echo 0x0200 > "$G/bcdUSB"
	echo 0xEF > "$G/bDeviceClass"		# Miscellaneous / IAD: needed by Windows for UVC
	echo 0x02 > "$G/bDeviceSubClass"
	echo 0x01 > "$G/bDeviceProtocol"
	mkdir -p "$G/strings/0x409"
	SERIAL=$(awk '/^Serial/ {print $3}' /proc/cpuinfo 2>/dev/null)
	echo "${SERIAL:-0000000000000001}" > "$G/strings/0x409/serialnumber"
	echo "SPRUT" > "$G/strings/0x409/manufacturer"
	echo "SPRUT-M CVBS grabber" > "$G/strings/0x409/product"

	mkdir -p "$G/configs/c.1/strings/0x409"
	echo "UVC" > "$G/configs/c.1/strings/0x409/configuration"
	echo 500 > "$G/configs/c.1/MaxPower"

	mkdir -p "$F"
	# control interface: no camera / processing controls (older kernels keep these read-only: ignore errors)
	echo 0 > "$F/control/terminal/camera/default/bmControls" 2>/dev/null || true
	echo 0 > "$F/control/processing/default/bmControls" 2>/dev/null || true
	mkdir -p "$F/control/header/h"
	ln -s "$F/control/header/h" "$F/control/class/fs/h"
	ln -s "$F/control/header/h" "$F/control/class/ss/h"

	# streaming: uncompressed YUY2 (the default guidFormat of an uncompressed format), one frame
	U="$F/streaming/uncompressed/u"
	mkdir -p "$U/${H}p"
	echo $W > "$U/${H}p/wWidth"
	echo $H > "$U/${H}p/wHeight"
	echo $BUF > "$U/${H}p/dwMaxVideoFrameBufferSize"
	echo $RATE > "$U/${H}p/dwMinBitRate"
	echo $RATE > "$U/${H}p/dwMaxBitRate"
	echo $IVAL > "$U/${H}p/dwDefaultFrameInterval"
	echo $IVAL > "$U/${H}p/dwFrameInterval"
	mkdir -p "$F/streaming/header/h"
	ln -s "$U" "$F/streaming/header/h/u"
	ln -s "$F/streaming/header/h" "$F/streaming/class/fs/h"
	ln -s "$F/streaming/header/h" "$F/streaming/class/hs/h"
	ln -s "$F/streaming/header/h" "$F/streaming/class/ss/h"

	# 720x576x2x25 = 20.7 MB/s: high-bandwidth isochronous, 3 x 1024 bytes every 125 us (24.5 MB/s)
	echo 3072 > "$F/streaming_maxpacket"
	echo 1 > "$F/streaming_interval"

	ln -s "$F" "$G/configs/c.1/uvc.0"
	UDC=$(ls /sys/class/udc | head -n1)
	if [ -z "$UDC" ]; then
		echo "no USB device controller: add dtoverlay=dwc2,dr_mode=peripheral to config.txt and reboot" >&2
		exit 1
	fi
	echo "$UDC" > "$G/UDC"
	echo "UVC gadget up on $UDC: ${W}x${H} YUY2, interval $IVAL x100 ns"
}

case "${1:-start}" in
	start) start ;;
	stop) stop ;;
	*) echo "usage: $0 [start|stop]" >&2; exit 2 ;;
esac
