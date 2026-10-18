#!/bin/sh
# Flashrom 编程器启动器 (C 版, X11+Xft, 中文正常)
cd "$(dirname "$0")" || exit 1
exec ./flashrom-gui-c "$@"
