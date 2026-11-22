#!/bin/sh
# Install the SPRUT-M grabber on a Raspberry Pi 4 (Raspberry Pi OS, Bookworm or newer):
# USB webcam (USB-C to the laptop) and a web page: http://169.254.10.1/ over a LAN cable, http://adv.local/ over
# Wi-Fi or a router; and, with EXTRA_ARGS=--hdmi, the Pi's own HDMI port (lowest delay); all at once.
# Usage:  sudo sh install.sh [pal|ntsc]        then reboot.
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh install.sh" >&2; exit 1; }
STD=${1:-pal}
case "$STD" in pal|ntsc) ;; *) echo "standard must be pal or ntsc" >&2; exit 2 ;; esac
cd "$(dirname "$0")"

echo "== packages"
apt-get install -y build-essential linux-libc-dev v4l-utils i2c-tools libdrm-dev

echo "== self-test"
gcc -O2 -Wall -o /tmp/sprutm-test_weave test_weave.c && /tmp/sprutm-test_weave && rm -f /tmp/sprutm-test_weave

echo "== build"
# -O3: the HDMI output's per-line loops are vectorized (NEON); SPRUTM_HDMI + libdrm: the HDMI output (hdmi.h)
gcc -O3 -Wall -Wextra -DSPRUTM_HDMI $(pkg-config --cflags libdrm) -o /usr/local/bin/sprutm-uvc sprutm-uvc.c \
	$(pkg-config --libs libdrm) -lpthread
install -m 755 sprutm-gadget.sh /usr/local/sbin/sprutm-gadget.sh
install -d /usr/local/share/sprutm
install -m 644 www/index.html /usr/local/share/sprutm/index.html
install -m 644 sprutm-gadget.service sprutm-uvc.service /etc/systemd/system/
rm -f /boot/firmware/overlays/sprutm-dwc2-fifo.dtbo /boot/overlays/sprutm-dwc2-fifo.dtbo	# old attempt (FIFO > 512 words is refused)

echo "== settings"
if [ -f /etc/default/sprutm-grabber ]; then
	sed -i "s/^STD=.*/STD=$STD/" /etc/default/sprutm-grabber
	grep -q '^FORMAT=' /etc/default/sprutm-grabber || printf '# Frame format: nv12 (fits the Pi 4 USB-C) or yuyv (does not)\nFORMAT=nv12\n' >> /etc/default/sprutm-grabber
	grep -q '^WEB_PORT=' /etc/default/sprutm-grabber || printf '# Web page port (0 = off): http://adv.local/\nWEB_PORT=80\n' >> /etc/default/sprutm-grabber
else
	sed "s/^STD=.*/STD=$STD/" sprutm-grabber.default > /etc/default/sprutm-grabber
fi
printf 'libcomposite\ni2c-dev\n' > /etc/modules-load.d/sprutm.conf

# LAN cable straight into the laptop: nobody hands out addresses, so Windows takes a link-local 169.254.x.x
# address. The Pi gets a fixed one in the same range, 169.254.10.1 (next to DHCP, which still works with a
# router), so the page is always http://169.254.10.1/ over the cable. Not <hostname>.local: that name also
# carries the Wi-Fi addresses and the browser may pick Wi-Fi. DHCP waits forever: with the default 45 s
# timeout NetworkManager restarts the link every 45 s and drops the addresses (the video stops).
if command -v nmcli >/dev/null 2>&1; then
	nmcli -t -f NAME,TYPE connection show | while IFS=: read -r name type; do
		[ "$type" = 802-3-ethernet ] || continue
		nmcli connection modify "$name" ipv4.link-local fallback ipv4.dhcp-timeout infinity 2>/dev/null || continue
		nmcli -g ipv4.addresses connection show "$name" | grep -q '169\.254\.10\.1/' ||
			nmcli connection modify "$name" +ipv4.addresses 169.254.10.1/16
		echo "   $name: cable address 169.254.10.1"
	done
fi

CFG=/boot/firmware/config.txt
[ -f "$CFG" ] || CFG=/boot/config.txt
[ -f "$CFG.sprutm-bak" ] || cp "$CFG" "$CFG.sprutm-bak"
sed -i '/^# >>> sprutm-grabber/,/^# <<< sprutm-grabber/d' "$CFG"
sed -i 's/^camera_auto_detect=1/#&  (sprutm-grabber)/; s/^dtoverlay=adv728x-m/#&/' "$CFG"
cat >> "$CFG" <<'EOF'
# >>> sprutm-grabber
[all]
camera_auto_detect=0
dtoverlay=adv728x-m,adv7280m=1,media-controller
dtoverlay=dwc2,dr_mode=peripheral
# <<< sprutm-grabber
EOF

systemctl daemon-reload
systemctl enable sprutm-gadget.service sprutm-uvc.service
echo "== done: standard $STD, config.txt updated (backup: $CFG.sprutm-bak). Now: sudo reboot"
echo "   after the reboot: USB webcam \"UVC Camera\"; the page: http://169.254.10.1/ (LAN cable),"
echo "   http://$(hostname).local/ (Wi-Fi or router); HDMI: add --hdmi to EXTRA_ARGS in /etc/default/sprutm-grabber"
