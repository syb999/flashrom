#!/bin/sh
# 构建脚本: 生成静态 flashrom + 静态 GUI (结果: ./flashrom, ./flashrom-gui-c)
set -e
cd "$(dirname "$0")"
export PATH="$HOME/.local/bin:$PATH"

echo "== 1/3 配置并编译 flashrom (尽量静态) =="
[ -d buildstatic2 ] || meson setup buildstatic2 --buildtype=release \
        --prefer-static -Dc_link_args="-static"
ninja -C buildstatic2 flashrom || true     # 末尾 -ludev 静态库缺失会失败, 属预期

echo "== 2/3 手动链接 flashrom (仅 libudev 动态, 其余静态) =="
cd buildstatic2
cc -o ../flashrom \
   flashrom.p/cli_classic.c.o flashrom.p/cli_common.c.o flashrom.p/cli_output.c.o \
   -Wl,--no-undefined -Wl,-O1 \
   -Wl,-Bstatic -Wl,--start-group libflashrom.a \
       /usr/lib/x86_64-linux-gnu/libcrypto.a \
       /usr/lib/x86_64-linux-gnu/libusb-1.0.a \
   -Wl,--end-group -Wl,-Bdynamic -ludev -lpthread -ldl -lm
cd ..

echo "== 3/3 编译 GUI (全静态) =="
gcc -O2 -static -I/usr/include/freetype2 -o flashrom-gui-c flashrom-gui-c.c \
    -Wl,--start-group \
    -lXft -lXrender -lXext -lX11 -lxcb -lXau -lXdmcp \
    -lfontconfig -lfreetype -lexpat -luuid -lpng16 -lbz2 -lz \
    -lbrotlidec -lbrotlicommon -lm -lpthread -ldl \
    -Wl,--end-group

strip flashrom flashrom-gui-c 2>/dev/null || true
echo "OK: ./flashrom  ./flashrom-gui-c"
