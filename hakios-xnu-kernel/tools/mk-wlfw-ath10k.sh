#!/bin/bash
# Give the kernel's build ath10k's descriptions of the messages a WCN3990's firmware speaks (WLFW, QMI service
# 0x45), for drivers/wifi/hakios.wcn3990.c: the Mi 9's (cepheus) Wi-Fi, whose firmware runs in the modem's WLAN PD.
# They are ath10k's qmi_wlfw_v01.h and the tables of its qmi_wlfw_v01.c as they are, and the configuration of the copy
# engines that snoc.c gives the firmware (its three target tables, with what of ce.h, htc.h and qmi.h they are written
# in), put into one header in the kernel's build directory, where the compiler finds it and git does not -- as tools/mk-wlfw.sh does with ath11k's
# (ath10k's files are ISC; they are not kept in the repository all the same). Without that header hakios.wcn3990.c
# builds its steps 1 to 3 only and says so at step 4.
#
# The sources come from the Linux release tools/mk-ath11k.sh pins, from the archive it keeps in
# xnu-deps/build/ath11k (fetched by it, "tools/mk-ath11k.sh fetch", when it is not there).
#
# usage: tools/mk-wlfw-ath10k.sh          then build the kernel (make kernel); again after a clean build
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
[ "$(uname)" = Darwin ] && PATH=$ROOT/tools/macos-bin:$PATH     # GNU sha256sum and tar, as xl takes them
source "$ROOT/xnu-env.sh"
VERSION=$(sed -n 's/^VERSION=//p' "$ROOT/tools/mk-ath11k.sh")
TARBALL=$ROOT/xnu-deps/build/ath11k/linux-$VERSION.tar.xz
[ -f "$TARBALL" ] || "$ROOT/tools/mk-ath11k.sh" fetch
SRC=$ROOT/xnu-deps/build/ath10k/src
FILES="qmi_wlfw_v01.c qmi_wlfw_v01.h snoc.c ce.h htc.h qmi.h"
if [ "$(cat "$SRC/.release" 2>/dev/null)" != "$VERSION $FILES" ]; then
	rm -rf "$SRC" "$SRC.unpacked"
	mkdir -p "$SRC.unpacked"
	tar xJf "$TARBALL" -C "$SRC.unpacked" --strip-components=6 \
		$(for f in $FILES; do echo linux-$VERSION/drivers/net/wireless/ath/ath10k/$f; done)
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in $FILES; do
		f=$SRC.unpacked/$f
		head -1 "$f" | grep -q 'SPDX-License-Identifier: ISC' || { echo "mk-wlfw-ath10k.sh: $f is not ISC" >&2; exit 1; }
	done
	mv "$SRC.unpacked" "$SRC"
	echo "$VERSION $FILES" > "$SRC/.release"
fi
OUT=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
[ -d "$OUT" ] || { echo "mk-wlfw-ath10k.sh: no build directory $OUT: build the kernel once first" >&2; exit 1; }
python3 - "$SRC" "$VERSION" > "$OUT/hakios.wlfw_ath10k.h.new" <<'PY'
import re
import sys

src, release = sys.argv[1], sys.argv[2]
c = open(src + '/qmi_wlfw_v01.c').read()
h = open(src + '/qmi_wlfw_v01.h').read()
print('/* ath10k\'s qmi_wlfw_v01.h and the tables of its qmi_wlfw_v01.c (Linux %s, ISC), put together by' % release)
print(' * tools/mk-wlfw-ath10k.sh: not to be edited, not to be kept. */')
print('''#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
''')
h = re.sub(r'^#include [<"][^>"]*[>"]\n', '', h, flags=re.M)
h = re.sub(r'^extern const struct qmi_elem_info \w+\[\];\n', '', h, flags=re.M)
print(h)
# every table, in the file's order (a table is used by the ones after it), each of this file alone
tables = re.findall(r'^(?:static )?const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S | re.M)
print('\n'.join(t if t.startswith('static ') else 'static ' + t for t in tables))

# snoc.c's configuration of the copy engines for the firmware (ath10k_snoc_wlan_enable()), and what it is written in
def one(text, pattern, what):
	found = re.search(pattern, text, re.S | re.M)
	if not found:
		sys.exit('mk-wlfw-ath10k.sh: no %s' % what)
	return found.group(0)
ce = open(src + '/ce.h').read()
htc = open(src + '/htc.h').read()
qmi = open(src + '/qmi.h').read()
snoc = open(src + '/snoc.c').read()
print('/* snoc.c\'s configuration of the copy engines, as the firmware is given it */')
print('#define __cpu_to_le32(x) ((uint32_t)(x))\n#define __cpu_to_le16(x) ((uint16_t)(x))\n#define BIT(n) (1u << (n))')
print('typedef uint16_t __le16;')
print(one(ce, r'^#define CE_ATTR_DIS_INTR\b.*?$', 'CE_ATTR_DIS_INTR'))
print(one(ce, r'^#define CE_ATTR_FLAGS\b.*?$', 'CE_ATTR_FLAGS'))
print('\n'.join(re.findall(r'^#define PIPEDIR_\w+\s+\d+', ce, re.M)))
print(one(ce, r'^struct ce_pipe_config \{.*?\n\};', 'struct ce_pipe_config'))
print(one(ce, r'^struct ce_service_to_pipe \{.*?\n\};', 'struct ce_service_to_pipe'))
print(one(qmi, r'^struct ath10k_shadow_reg_cfg \{.*?\n\};', 'struct ath10k_shadow_reg_cfg'))
print(one(htc, r'^enum ath10k_htc_svc_gid \{.*?\n\};', 'the HTC service groups'))
print(one(htc, r'^#define SVC\(.*?\n#undef SVC', 'the HTC services'))
print('\n'.join(re.findall(r'^#define WCN3990_\w+_WR_IDX_OFFSET.*$', snoc, re.M)))
for name in ('target_shadow_reg_cfg_map', 'target_ce_config_wlan', 'target_service_to_ce_map_wlan'):
	table = one(snoc, r'^static struct \w+ ' + name + r'\[\] = \{.*?\n\};', name)
	print('static const ' + table[len('static '):])
PY
cmp -s "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h" 2>/dev/null && rm "$OUT/hakios.wlfw_ath10k.h.new" ||
	mv "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h"
echo "mk-wlfw-ath10k.sh: $OUT/hakios.wlfw_ath10k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/hakios.wlfw_ath10k.h") tables of messages, 3 of the copy engines)"
