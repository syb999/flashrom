#!/bin/sh
# flashrom 特权助手: 由 GUI 用 pkexec 拉起一次(只授权一次),
# 之后通过命名管道接收命令并执行, 输出写到 $D/out.txt
# 用法: helper.sh <workdir>
D="$1"
[ -d "$D" ] || exit 1
export LC_ALL=C

while :; do
    if read -r CMD < "$D/cmd"; then
        [ "$CMD" = "__QUIT__" ] && exit 0
        : > "$D/out.txt"
        /bin/sh -c "$CMD" >> "$D/out.txt" 2>&1
        echo "__DONE__" >> "$D/out.txt"
    fi
done
