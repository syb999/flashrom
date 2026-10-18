#!/bin/sh
# 打包 .deb (需要先跑 ./build.sh)
set -e
cd "$(dirname "$0")"
V="${1:-2.1}"
B=/tmp/debbuild/flashrom-ch341a-gui

[ -x ./flashrom ] && [ -x ./flashrom-gui-c ] || { echo "先跑 ./build.sh"; exit 1; }

rm -rf "$B"
mkdir -p "$B/DEBIAN" "$B/opt/flashrom-ch341a" \
         "$B/usr/bin" "$B/usr/share/applications" \
         "$B/usr/share/doc/flashrom-ch341a-gui" "$B/lib/udev/rules.d"

cp flashrom flashrom-gui-c flashrom-helper.sh "$B/opt/flashrom-ch341a/"
chmod 755 "$B/opt/flashrom-ch341a/"*
cp 99-ch341a-nopass.rules "$B/lib/udev/rules.d/"
chmod 644 "$B/lib/udev/rules.d/99-ch341a-nopass.rules"

printf '#!/bin/sh\nexec /opt/flashrom-ch341a/flashrom-gui-c "$@"\n' \
    > "$B/usr/bin/flashrom-ch341a-gui"
chmod 755 "$B/usr/bin/flashrom-ch341a-gui"

cat > "$B/usr/share/applications/flashrom-ch341a-gui.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Flashrom 编程器 (CH341A)
Comment=CH341A SPI 闪存检测/备份/写入/只写u-boot区
Exec=/usr/bin/flashrom-ch341a-gui
Icon=drive-harddisk
Terminal=false
Categories=Utility;System;
Keywords=flashrom;ch341a;spi;bios;uboot;art;
EOF

cat > "$B/DEBIAN/control" <<EOF
Package: flashrom-ch341a-gui
Version: $V
Section: utils
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.17), libudev1, sudo | policykit-1, xclip | xsel
Maintainer: syb999 <syb999@localhost>
Installed-Size: 9800
Description: CH341A SPI flash programmer GUI (Flashrom)
 Graphical front-end for flashrom with a CH341A USB SPI programmer:
 detect / read / write / verify / erase, write only the u-boot region,
 build a blank image, file browser, log copy.
 Statically linked, runs on any x86_64 Linux (Linux Mint 17+).
 Installs a udev rule so the CH341A works without root.
EOF

cat > "$B/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
command -v udevadm >/dev/null 2>&1 && { udevadm control --reload-rules || true; udevadm trigger || true; }
update-desktop-database /usr/share/applications 2>/dev/null || true
# 桌面图标
put_icon() {
  u="$1"; h="$2"; [ -d "$h" ] || return 0
  for d in "$h/桌面" "$h/Desktop"; do
    [ -d "$d" ] || continue
    cp -f /usr/share/applications/flashrom-ch341a-gui.desktop "$d/" 2>/dev/null || continue
    chmod 755 "$d/flashrom-ch341a-gui.desktop"
    chown "$u":"$u" "$d/flashrom-ch341a-gui.desktop" 2>/dev/null || true
    su -s /bin/sh "$u" -c "gio set '$d/flashrom-ch341a-gui.desktop' metadata::trusted true" 2>/dev/null || true
  done
}
if [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != root ]; then
  put_icon "$SUDO_USER" "$(getent passwd "$SUDO_USER" | cut -d: -f6)"
else
  for h in /home/*; do
    [ -d "$h" ] || continue; u=$(basename "$h")
    [ "$(id -u "$u" 2>/dev/null || echo 0)" -ge 1000 ] && put_icon "$u" "$h"
  done
fi
exit 0
EOF

cat > "$B/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
command -v udevadm >/dev/null 2>&1 && { udevadm control --reload-rules || true; udevadm trigger || true; }
for h in /home/*; do
  rm -f "$h/桌面/flashrom-ch341a-gui.desktop" "$h/Desktop/flashrom-ch341a-gui.desktop" 2>/dev/null || true
done
exit 0
EOF
chmod 755 "$B/DEBIAN/postinst" "$B/DEBIAN/postrm"

cp README.md "$B/usr/share/doc/flashrom-ch341a-gui/README" 2>/dev/null || true

dpkg-deb --build "$B" "./flashrom-ch341a-gui_${V}_amd64.deb"
echo "OK: ./flashrom-ch341a-gui_${V}_amd64.deb"
