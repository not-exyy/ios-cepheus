#!/bin/sh
# The Mi 9's fuel-gauge driver (drivers/power/hakios.pmfg.c) on this machine: tools/pmfg_test.c built around it and run.
# The driver's own source is used, with its two register accessors renamed so that the test's pretend arbiter and
# PM8150B answer instead; the kernel headers it includes are empty files here (the test gives what they would).
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for h in kern/clock.h kern/thread.h libkern/libkern.h mach/mach_types.h machine/machine_routines.h pexpert/pexpert.h \
    sys/sysctl.h sys/systm.h; do
	mkdir -p "$tmp/inc/$(dirname $h)"
	: > "$tmp/inc/$h"
done
cp xnu/files/bsd/sys/hakios.input.h "$tmp/inc/sys/"
sed -e 's/^rd(vm_offset_t address)$/rd_hw(vm_offset_t address)/' \
    -e 's/^wr(vm_offset_t address, uint32_t value)$/wr_hw(vm_offset_t address, uint32_t value)/' \
    drivers/power/hakios.pmfg.c > "$tmp/pmfg_under_test.c"
[ "$(grep -c '^rd_hw(\|^wr_hw(' "$tmp/pmfg_under_test.c")" = 2 ] || { echo "pmfg-test: rd()/wr() of the driver not found"; exit 1; }
cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined \
    -I"$tmp/inc" -I"$tmp" tools/pmfg_test.c -lm -o "$tmp/pmfg_test"
"$tmp/pmfg_test"
