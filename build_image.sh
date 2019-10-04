#!/bin/bash
TARGET=mipsel-mtk-linux
ENTRY_POINT=$($TARGET-nm vmlinux 2>/dev/null | grep "\bkernel_entry\b" | cut -f1 -d ' ')

$TARGET-objcopy -O binary --remove-section=.mdebug --remove-section=.comment --remove-section=.note --remove-section=.pdr --remove-section=.options --remove-section=.MIPS.options vmlinux vmlinux.bin
./lzma e vmlinux.bin vmlinux.bin.lzma -lc1 -lp2 -pb2
./mkimage -A mips -O linux -T kernel -C lzma -a 0x81001000 -e 0x$($TARGET-nm vmlinux 2>/dev/null | grep "\bkernel_entry\b" | cut -f1 -d ' ') -n "Linux 3.10" -d ./vmlinux.bin.lzma uImage
