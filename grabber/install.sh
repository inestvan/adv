#!/bin/sh
# Install the SPRUT-M USB grabber on a Raspberry Pi 4 (Raspberry Pi OS, Bookworm or newer).
# Usage:  sudo sh install.sh [pal|ntsc]        then reboot.
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh install.sh" >&2; exit 1; }
STD=${1:-pal}
case "$STD" in pal|ntsc) ;; *) echo "standard must be pal or ntsc" >&2; exit 2 ;; esac
cd "$(dirname "$0")"

echo "== packages"
apt-get install -y build-essential linux-libc-dev v4l-utils i2c-tools

echo "== self-test"
gcc -O2 -Wall -o /tmp/sprutm-test_weave test_weave.c && /tmp/sprutm-test_weave && rm -f /tmp/sprutm-test_weave

echo "== build"
gcc -O2 -Wall -Wextra -o /usr/local/bin/sprutm-uvc sprutm-uvc.c
install -m 755 sprutm-gadget.sh /usr/local/sbin/sprutm-gadget.sh
install -m 644 sprutm-gadget.service sprutm-uvc.service /etc/systemd/system/

echo "== settings"
if [ -f /etc/default/sprutm-grabber ]; then
	sed -i "s/^STD=.*/STD=$STD/" /etc/default/sprutm-grabber
else
	sed "s/^STD=.*/STD=$STD/" sprutm-grabber.default > /etc/default/sprutm-grabber
fi
printf 'libcomposite\ni2c-dev\n' > /etc/modules-load.d/sprutm.conf

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
