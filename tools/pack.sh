#!/bin/bash
# The kit as it may be shared: hakios-mi9.tar.gz next to this folder, with only what anybody may have.
#
#   tools/pack.sh
#
# In it: README.md, setup.sh, run.sh, tools/, hakios-xnu-kernel's tracked files (and its history, when it is a clone of its own) and
# the Mi 9 port devices/cepheus without the device tree. NOT in it, whatever is lying here: what a build made from an
# IPSW (sysimage/, userland/*.apfs, xnuloader/identity.h, loader.efi, work/), the phone's firmware (fw-*/), its device
# tree, backups/, logs/. The archive is listed afterwards and refused if one of those got in.
set -euo pipefail
KIT=$(cd "$(dirname "$0")/.." && pwd)
NAME=$(basename "$KIT")
OUT=$(dirname "$KIT")/$NAME.tar.gz
cd "$(dirname "$KIT")"
list=$(mktemp)
trap 'rm -f "$list"' EXIT
{
	for f in README.md setup.sh run.sh; do echo "$NAME/$f"; done
	find "$NAME/tools" -type f ! -name '.*' ! -path '*/__pycache__/*'
	if [ -d "$NAME/hakios-xnu-kernel/.git" ]; then          # hakios-xnu-kernel its own clone
		git -C "$NAME/hakios-xnu-kernel" ls-files | sed "s|^|$NAME/hakios-xnu-kernel/|"
		find "$NAME/hakios-xnu-kernel/.git" -type f ! -path '*/.git/logs/*'      # not the reflog: this machine's moves
	else                                                      # the kit cloned from GitHub: its own tracked files
		git -C "$NAME" ls-files | sed "s|^|$NAME/|"
	fi
	find "$NAME/hakios-xnu-kernel/devices/cepheus" -type f ! -name device-tree.dtb ! -name '.*'
} | sort -u > "$list"
# a tracked file that is not there (deleted by hand) is left out rather than failing tar
while read -r f; do [ -e "$f" ] || echo "pack.sh: $f is missing, left out" >&2; done < "$list"
tar -czf "$OUT" --no-recursion -T <(while read -r f; do [ -e "$f" ] && echo "$f"; done < "$list")
bad=$(tar -tzf "$OUT" | grep -E '(^|/)(sysimage/(rootfs|cryptex|system\.files|DeviceTree)|fw-[a-z]+/|work/|backups/|(^|[^t])/logs/)|identity\.h$|/(root|system|flashboot)[^/]*\.apfs$|device-tree\.dtb$|\.ipsw$' || true)
if [ -n "$bad" ]; then
	rm -f "$OUT"
	echo "pack.sh: these must not be shared, nothing made:"; echo "$bad"; exit 1
fi
if tar -xzOf "$OUT" 2>/dev/null | LC_ALL=C grep -aqE '4c1dcc51|/Users/[a-z]'; then
	echo "pack.sh: WARNING: a phone serial or a /Users/ path is inside one of the files; look before sharing" >&2
fi
echo "$OUT: $(tar -tzf "$OUT" | wc -l | tr -d ' ') files, $(du -h "$OUT" | cut -f1)"
shasum -a 256 "$OUT" | tee "$OUT.sha256"
