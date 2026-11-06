#!/bin/bash
# mkfs.hfsplus for a plain file on macOS, for tools/mkdatavol.py (its MKFS argument):
#   tools/mkdatavol.py OUT.simg tools/newfs-hfs-file.sh DARWIN_BLOCKS
# macOS's newfs_hfs is the same program as Linux's mkfs.hfsplus, with the same options, but it formats only devices:
# the file is attached as a raw disk without mounting it, formatted there, and detached again. No root needed.
set -e
file=${@: -1}
[ -f "$file" ] || { echo "newfs-hfs-file.sh: $file: not a file" >&2; exit 1; }
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$file" 2>/dev/null | awk 'NR == 1 { print $1 }')
[ -n "$dev" ] || { echo "newfs-hfs-file.sh: $file could not be attached" >&2; exit 1; }
trap 'hdiutil detach "$dev" >/dev/null 2>&1' EXIT
/sbin/newfs_hfs "${@:1:$#-1}" "/dev/r${dev#/dev/}"
