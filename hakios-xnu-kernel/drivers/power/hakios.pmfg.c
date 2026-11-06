/*
 * The battery of the Xiaomi Mi 9 (cepheus, SM8150): the charge in percent, as Android shows it, from the
 * fuel gauge of the PM8150B -- Qualcomm's "FG gen4" (drivers/power/supply/qcom/qpnp-fg-gen4.c, fg-util.c and
 * fg-reg.h of Xiaomi's kernel, LineageOS android_kernel_xiaomi_sm8150) -- and whether a charger is on the
 * port and charging, from the same chip's charger (smb5-lib.c, smb5-reg.h).
 *
 * The SM7325 phone has its battery behind the audio DSP's battery manager (drivers/audio/hakios.adsp.c); the
 * Mi 9 has none of that. Its fuel gauge is a block of the PM8150B that runs by itself: Android only loads the
 * battery's profile into it once (it stays while the battery is connected: "Battery profile integrity bit is
 * set ... not loading it" in the TWRP log of 2026-11-02) and then reads its registers. So the charge is there
 * to be read through the SPMI arbiter, the way the keys (hakios.buttons.c) and the motor (hakios.pmhaptic.c)
 * reach their blocks. The reading goes to drivers/power/hakios.battery.c (hakios_battery_report()), which keeps
 * it for the sysctls kern.hakios.battery_* and gives it to HakiosBattery (hakios.battery.cpp), the
 * IOPMPowerSource that powerd reads: the status bar's battery, "Charging", Settings > Battery.
 *
 * THIS DRIVER ONLY READS. It writes nothing to a PMIC -- not the fuel gauge, not the charger: charging stays
 * as the boot loader and the chip's own defaults have it. Each read is one transfer through this system's
 * observer channel of the arbiter, under hakios_spmi_lock(), as the other SPMI drivers do it.
 *
 * Where everything is (the phone's device tree, recovery-log-20261102/Device-tree.dtb, qcom,pm8150b@2):
 *
 *   slave 2 0x4000   qpnp,fg qcom,fg-batt-soc   subtype 0x10 (FG_BATT_SOC_PM8150B)
 *           0x4100           qcom,fg-batt-info  subtype 0x11 (FG_BATT_INFO_PM8150B)
 *           0x4200           qcom,fg-rradc      subtype 0x13 (FG_ADC_RR_PM8150B)
 *           0x1000   qcom,qpnp-smb5 chgr;  0x1300 usb;  0x1400 dc (the Mi 9 charges without a wire too)
 *   the battery      /vendor/qcom,battery-data: 3300 mAh (qcom,nom-batt-capacity-mah 0xce4), 4400 mV full
 *
 * What is read, and how Linux makes it into what Android shows:
 *
 *   the charge       BATT_SOC 0x09, 0x0a: the "monotonic SOC" and its copy, 0..255; read until the two agree
 *                    (fg_get_msoc_raw()); 255 is 100%, 0 is 0%, 1..254 are 1..99% (fg_get_msoc());
 *                    full (100%) when the charger has terminated and the charge is 99% or more
 *                    (fg_gen4_charge_full_update()); not reported before the gauge says SOC_READY
 *                    (BATT_SOC 0x10 bit 1; Linux shows 50% until then)
 *   the voltage      BATT_INFO 0xa0..0xa1, with its copy at 0xa6..0xa7: 122.07 uV a step (fg_get_battery_voltage())
 *   the current      BATT_INFO 0xa2..0xa3, copy 0xa8..0xa9: signed, 488.281 uA a step, more than 0 while the
 *                    battery gives charge (fg_get_battery_current()); Darwin wants the other sign
 *   the battery      RRADC 0x10 bit 0: the battery is missing (fg_gen4_battery_missing ... ADC_RR_BT_MISS_BIT)
 *   the charger      USBIN 0x10 bit 4 and DCIN 0x10 bit 4: something is plugged in (USBIN/DCIN_PLUGIN_RT_STS);
 *                    CHGR 0x06 [2:0]: trickle, pre, full-on, taper = charging; terminate, inhibit = full;
 *                    pause, disable = not charging (smblib_get_prop_batt_status())
 *   the temperature  Linux reads it from the gauge's SRAM (fg_gen4_get_battery_temp()), which takes writes
 *                    to its memory interface; that is not done here, and Darwin is told it is not known.
 *                    RRADC 0x88..0x89 (ADC_RR_BATT_TEMP, 10 bits) is read for the log only: on the phone
 *                    (logs/104324-run.log, charging, a room) it read 0x19..0x1a -- not the SRAM word's quarters of a
 *                    degree (6.25 C), perhaps whole degrees; until that is checked against Android it is not used
 *
 * What Android does and this does not: the "SOC scale" below 3400 mV (soc-scale-mode-en: a slower fall of the
 * shown charge near empty), Xiaomi's shutdown delay, the learnt capacity and the cycle count (both in SRAM).
 * Near empty the percentage here can therefore be a little below Android's.
 *
 * Boot-args:
 *   fuelgauge=1   find the blocks, check what they are, and read them every fuelgauge_ms (default 5000) ms,
 *                 a line in the log every 30 s or when the charge or the charger changes; sysctls
 *                 kern.hakios.fg_*. Nothing goes to Darwin.
 *   fuelgauge=2   ... and Darwin is told (hakios_battery_report()): the battery in iOS's status bar.
 *
 * Status: tools/pmfg-test.sh (this file against a pretend arbiter and PM8150B on the Mac, 51 checks: Linux's
 * percentages for all 256 raw values, the current's sign, the charger's states, full, the plug-in bits settling, a copy
 * that disagrees, a refusal, another subtype; only reads, each under the lock) passes.
 * On the phone, fuelgauge=1 (2026-11-05, logs/104324-run.log, 60 s of iOS on the Mac's USB): the arbiter 0x50000000; the
 * blocks are channels 283/284/285 (type 0x0d, subtypes 0x10/0x11/0x13), chgr/usb/dc 266/269/270 (type 0x02, subtypes
 * 0x80/0x83/0x84); 100% (raw 255), 4418-4419 mV, +159..173 mA, charger "taper"; 11 readings, none failed; usb read 0 once.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

static uint32_t level, interval_ms = 5000;

/* every line to the loader's log as well (and to the console when that may be written) */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter (version 5), reads only, as hakios.buttons.c and hakios.pmhaptic.c have it ---------- */

#define ARB_CORE_PHYS           0x0c440000UL    /* "core": the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* "obsrvr": 0x10000 per system, 0x80 per channel in it */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral */
#define ARB_MAX_CHANNELS        512

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */
#define TRANSFER_TIMEOUT_US     2000

/* the blocks, slave 2 (the PM8150B's first): what the arbiter calls them ([19:8] of its map), what they should be */
enum { B_SOC, B_INFO, B_RRADC, B_CHGR, B_USBIN, B_DCIN, B_COUNT };
static const struct block {
	const char *name;
	uint32_t peripheral;
	int subtype;            /* -1: not checked (Linux does not check it either) */
	bool needed;
} blocks[B_COUNT] = {
	[B_SOC]   = { "fg-batt-soc",  0x240, 0x10, true },
	[B_INFO]  = { "fg-batt-info", 0x241, 0x11, true },
	[B_RRADC] = { "fg-rradc",     0x242, 0x13, false },
	[B_CHGR]  = { "smb5 chgr",    0x210, -1,   false },
	[B_USBIN] = { "smb5 usb",     0x213, -1,   false },
	[B_DCIN]  = { "smb5 dc",      0x214, -1,   false },
};
static uint32_t channels[B_COUNT];
static bool found[B_COUNT];
static vm_offset_t arb_core, arb_observer;

static const char *
status_problem(uint32_t status)
{
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		return "the transfer failed";
	} else if (status & STATUS_DROPPED) {
		return "the transfer was dropped";
	}
	return NULL;
}

/* Read `count' (1 to 8) registers of block `b' from `reg' on. Returns NULL, or what went wrong. */
static const char *
spmi_read(int b, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t c;
	uint64_t start, timeout;
	uint32_t status, data[2];
	const char *problem;

	if (!found[b]) {
		return "the block has no channel";
	}
	c = arb_observer + 0x80UL * channels[b];
	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &timeout);
	hakios_spmi_lock();
	start = mach_absolute_time();
	wr(c + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(c + OBSERVER_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < timeout);
	data[0] = rd(c + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(c + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();
	if ((problem = status_problem(status)) == NULL) {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	return problem;
}

static bool
find_blocks(void)
{
	uint32_t version, count;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS + 0x10000UL * ARB_SYSTEM, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	count = rd(arb_core + ARB_FEATURES) & 0x7ff;
	say("fuelgauge: the PMIC arbiter is version %#x, %u channels\n", version, count);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("fuelgauge: not the arbiter this driver knows (version 5): nothing is done\n");
		return false;
	}
	if (count > ARB_MAX_CHANNELS) {
		count = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < count; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		for (int b = 0; b < B_COUNT; b++) {
			if (map != 0 && !found[b] && ((map >> 8) & 0xfff) == blocks[b].peripheral) {
				channels[b] = n;
				found[b] = true;
			}
		}
	}
	for (int b = 0; b < B_COUNT; b++) {
		uint8_t id[2] = { 0, 0 };
		const char *problem;

		if (!found[b]) {
			say("fuelgauge: %s (%#x00): no channel of the arbiter is it%s\n", blocks[b].name,
			    blocks[b].peripheral, blocks[b].needed ? ": nothing is done" : "");
			if (blocks[b].needed) {
				return false;
			}
			continue;
		}
		/* registers 0x04 and 0x05: the block's type and subtype */
		if ((problem = spmi_read(b, 0x04, id, 2)) != NULL) {
			say("fuelgauge: %s: type: %s\n", blocks[b].name, problem);
			found[b] = false;
			if (blocks[b].needed) {
				return false;
			}
			continue;
		}
		say("fuelgauge: %s is channel %u (%#x00), type %#x subtype %#x\n", blocks[b].name, channels[b],
		    blocks[b].peripheral, id[0], id[1]);
		if (blocks[b].subtype >= 0 && id[1] != blocks[b].subtype) {
			say("fuelgauge: %s: subtype %#x, not the PM8150B's %#x%s\n", blocks[b].name, id[1],
			    blocks[b].subtype, blocks[b].needed ? ": nothing is done" : ": not used");
			found[b] = false;
			if (blocks[b].needed) {
				return false;
			}
		}
	}
	return true;
}

/* ---- the fuel gauge and the charger: Linux's fg-util.c, qpnp-fg-gen4.c, smb5-lib.c -------------------- */

#define REG_INT_RT_STS          0x10            /* every block: its interrupts' live state */
#define SOC_READY_BIT           0x02            /* BATT_SOC */
#define SOC_MONOTONIC           0x09            /* BATT_SOC: the charge, 0..255, and its copy at 0x0a */
#define INFO_VBATT              0xa0            /* BATT_INFO: the voltage, little-endian; its copy at 0xa6 */
#define INFO_IBATT              0xa2            /* BATT_INFO: the current; its copy at 0xa8 */
#define INFO_COPY               0x06            /* from a value to its copy */
#define RRADC_BT_MISS_BIT       0x01
#define RRADC_BATT_TEMP         0x88            /* [9:0] over two registers */
#define CHGR_STATUS_1           0x06            /* [2:0] what the charger is doing */
#define PLUGIN_BIT              0x10            /* USBIN, DCIN */
#define READ_TRIES              5               /* MAX_READ_TRIES */
#define FULL_SOC_RAW            255
#define VBATT_NUMR              122070          /* uV a step, times 1000 */
#define IBATT_NUMR              488281          /* uA a step, times 1000 */
#define DESIGN_UAH              3300000         /* qcom,nom-batt-capacity-mah */
#define TEMPERATURE_UNKNOWN     INT32_MIN

enum { INHIBIT_CHARGE, TRICKLE_CHARGE, PRE_CHARGE, FULLON_CHARGE, TAPER_CHARGE, TERMINATE_CHARGE, PAUSE_CHARGE,
       DISABLE_CHARGE };
enum { STATUS_UNKNOWN, STATUS_CHARGING, STATUS_DISCHARGING, STATUS_NOT_CHARGING, STATUS_FULL };

static const char *const charger_states[8] = { "inhibit", "trickle", "pre", "full-on", "taper", "terminate",
	                                       "pause", "disable" };
static const char *const statuses[5] = { "unknown", "charging", "discharging", "not charging", "full" };

struct reading {
	int raw, percent, voltage_uv, current_ua, temperature_hundredths, temperature_raw;
	bool ready, missing, usb, dc;
	int charger, status;
};

static int fg_raw = -1, fg_percent = -1, fg_ready, fg_voltage_mv = -1, fg_current_ma, fg_temperature_raw = -1;
static int fg_status, fg_external;
static uint64_t fg_reads, fg_failures;

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's */
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_soc_raw, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_raw, 0, "the PM8150B fuel gauge's monotonic SOC, 0..255, or -1 before the first reading");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_percent, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_percent, 0, "the charge as Android would show it, 0..100, or -1");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_ready, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_ready, 0, "1 when the fuel gauge says its SOC is ready");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_voltage_mv, 0, "the battery's voltage by the fuel gauge, mV");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_current_ma, 0, "the battery's current by the fuel gauge, mA, more than 0 while it charges");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_temperature_raw, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_temperature_raw, 0, "RRADC BATT_TEMP as read, [9:0], or -1 (its scale is not yet checked)");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_status, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_status, 0, "Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full");
SYSCTL_INT(_kern_hakios, OID_AUTO, fg_external, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_external, 0, "1 while a charger is plugged in (a wire or the wireless pad)");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, fg_reads, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_reads, "readings of the fuel gauge");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, fg_failures, CTLFLAG_RD | CTLFLAG_LOCKED,
    &fg_failures, "readings that failed (a transfer, or a value and its copy that kept disagreeing)");

/* a value of two bytes and its copy, `copy' registers further on: read until they agree, as Linux does */
static const char *
read_pair(int b, uint8_t reg, uint8_t copy, uint16_t *value)
{
	uint8_t v[2], c[2];
	const char *problem;

	for (int tries = 0; tries < READ_TRIES; tries++) {
		if ((problem = spmi_read(b, reg, v, 2)) != NULL || (problem = spmi_read(b, reg + copy, c, 2)) != NULL) {
			return problem;
		}
		if (v[0] == c[0] && v[1] == c[1]) {
			*value = (uint16_t)(v[1] << 8 | v[0]);
			return NULL;
		}
	}
	return "the value and its copy do not agree";
}

/* fg_get_msoc(): 0 and 255 are the ends; 1..254 are spread over 1..99 */
static int
percent_of(int raw)
{
	if (raw >= FULL_SOC_RAW) {
		return 100;
	} else if (raw <= 0) {
		return 0;
	}
	return ((raw - 1) * 98 + (FULL_SOC_RAW - 2) / 2) / (FULL_SOC_RAW - 2) + 1;
}

static void decide(struct reading *r);

static const char *
read_all(struct reading *r)
{
	uint8_t soc[2], byte;
	uint16_t value;
	const char *problem;
	int tries;

	memset(r, 0, sizeof(*r));
	r->temperature_hundredths = TEMPERATURE_UNKNOWN;
	r->temperature_raw = -1;
	r->charger = -1;

	/* the charge: the monotonic SOC and its copy, until they agree (fg_get_msoc_raw()) */
	for (tries = 0; tries < READ_TRIES; tries++) {
		if ((problem = spmi_read(B_SOC, SOC_MONOTONIC, soc, 2)) != NULL) {
			return problem;
		}
		if (soc[0] == soc[1]) {
			break;
		}
	}
	if (tries == READ_TRIES) {
		return "MSOC: the value and its copy do not agree";
	}
	r->raw = soc[0];
	if ((problem = spmi_read(B_SOC, REG_INT_RT_STS, &byte, 1)) != NULL) {
		return problem;
	}
	r->ready = (byte & SOC_READY_BIT) != 0;

	if ((problem = read_pair(B_INFO, INFO_VBATT, INFO_COPY, &value)) != NULL) {
		return problem;
	}
	r->voltage_uv = (int)(((uint64_t)value * VBATT_NUMR) / 1000);
	if ((problem = read_pair(B_INFO, INFO_IBATT, INFO_COPY, &value)) != NULL) {
		return problem;
	}
	/* Linux: more than 0 while discharging; Darwin (and struct hakios_battery_state): more than 0 while charging */
	r->current_ua = -(int)(((int64_t)(int16_t)value * IBATT_NUMR) / 1000);

	if (found[B_RRADC]) {
		uint8_t t[2];

		if (spmi_read(B_RRADC, REG_INT_RT_STS, &byte, 1) == NULL) {
			r->missing = (byte & RRADC_BT_MISS_BIT) != 0;
		}
		if (spmi_read(B_RRADC, RRADC_BATT_TEMP, t, 2) == NULL) {
			/* only for the log and kern.hakios.fg_temperature_raw: its scale is not known (see the head of the file) */
			r->temperature_raw = ((t[1] & 0x03) << 8) | t[0];
		}
	}

	if (found[B_USBIN] && spmi_read(B_USBIN, REG_INT_RT_STS, &byte, 1) == NULL) {
		r->usb = (byte & PLUGIN_BIT) != 0;
	}
	if (found[B_DCIN] && spmi_read(B_DCIN, REG_INT_RT_STS, &byte, 1) == NULL) {
		r->dc = (byte & PLUGIN_BIT) != 0;
	}
	if (found[B_CHGR] && spmi_read(B_CHGR, CHGR_STATUS_1, &byte, 1) == NULL) {
		r->charger = byte & 0x07;
	}
	decide(r);
	return NULL;
}

/* What the charge and the charger's state come to, from a reading's registers (again after settle() has changed them) */
static void
decide(struct reading *r)
{
	r->percent = percent_of(r->raw);
	/* smblib_get_prop_batt_status() */
	switch (r->charger) {
	case TRICKLE_CHARGE: case PRE_CHARGE: case FULLON_CHARGE: case TAPER_CHARGE:
		r->status = (r->usb || r->dc) ? STATUS_CHARGING : STATUS_DISCHARGING;
		break;
	case TERMINATE_CHARGE: case INHIBIT_CHARGE:
		r->status = STATUS_FULL;
		break;
	case PAUSE_CHARGE: case DISABLE_CHARGE:
		r->status = (r->usb || r->dc) ? STATUS_NOT_CHARGING : STATUS_DISCHARGING;
		break;
	default:
		r->status = (r->usb || r->dc) ? STATUS_UNKNOWN : STATUS_DISCHARGING;
		break;
	}
	/* fg_gen4_charge_full_update(): the charger has finished and the charge is 99% or more: that is 100% */
	if ((r->usb || r->dc) && r->charger == TERMINATE_CHARGE &&
	    (r->raw * 100 + FULL_SOC_RAW / 2) / FULL_SOC_RAW >= 99) {
		r->percent = 100;
	}
}

/*
 * Whether something is plugged in, as the reading before agrees. On the phone (logs/104324-run.log) USBIN's plug-in bit
 * read 0 once in eleven readings while the battery was still taking 161 mA from the Mac's port; Darwin would have shown
 * "not charging" for five seconds. So a change of the two bits is taken only when two readings in a row say it.
 */
static void
settle(struct reading *r)
{
	static int settled = -1, pending = -1;
	int now = (r->usb ? 1 : 0) | (r->dc ? 2 : 0);

	if (settled < 0 || now == settled) {
		settled = now;
		pending = -1;
	} else if (now == pending) {
		settled = now;
		pending = -1;
	} else {
		pending = now;
	}
	if (now != settled) {
		r->usb = (settled & 1) != 0;
		r->dc = (settled & 2) != 0;
		decide(r);
	}
}

static void
tell_darwin(const struct reading *r)
{
	struct hakios_battery_state s;

	memset(&s, 0, sizeof(s));
	s.size = sizeof(s);
	s.percent_hundredths = r->percent * 100;
	s.voltage_uv = r->voltage_uv;
	s.current_ua = r->current_ua;
	s.temperature_hundredths = r->temperature_hundredths;
	s.present = !r->missing;
	s.external = r->usb || r->dc;
	s.status = r->status;
	s.charge_uah = (int)(((int64_t)DESIGN_UAH * r->raw) / FULL_SOC_RAW);
	s.design_uah = DESIGN_UAH;
	s.usb_type = 0;         /* not known: Darwin is told "USB, unknown" */
	hakios_battery_report(&s);
}

static void
fg_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct reading r, last;
	uint64_t last_line = 0, last_report = 0;
	bool have_last = false, reported = false;

	for (;;) {
		const char *problem = read_all(&r);
		uint64_t now = mach_absolute_time(), ns, report_ns;

		absolutetime_to_nanoseconds(now - last_line, &ns);
		absolutetime_to_nanoseconds(now - last_report, &report_ns);
		if (problem != NULL) {
			fg_failures++;
			if (fg_failures <= 10 || fg_failures % 100 == 0) {
				say("fuelgauge: a reading failed: %s (%llu failed, %llu good)\n", problem, fg_failures, fg_reads);
			}
		} else {
			settle(&r);
			bool changed = !have_last || r.percent != last.percent || r.status != last.status ||
			    r.usb != last.usb || r.dc != last.dc || r.ready != last.ready || r.missing != last.missing;

			fg_reads++;
			fg_raw = r.raw;
			fg_percent = r.percent;
			fg_ready = r.ready;
			fg_voltage_mv = r.voltage_uv / 1000;
			fg_current_ma = r.current_ua / 1000;
			fg_temperature_raw = r.temperature_raw;
			fg_status = r.status;
			fg_external = r.usb || r.dc;
			if (changed || ns >= 30ULL * NSEC_PER_SEC) {
				say("fuelgauge: %d%% (raw %d%s), %d mV, %d mA, battery %s, RRADC temperature register %#x; "
				    "usb %d dc %d, charger %s: %s (%llu readings, %llu failed)\n",
				    r.percent, r.raw, r.ready ? "" : ", NOT READY", r.voltage_uv / 1000, r.current_ua / 1000,
				    r.missing ? "MISSING" : "present", r.temperature_raw, r.usb, r.dc,
				    r.charger >= 0 ? charger_states[r.charger] : "not read", statuses[r.status],
				    fg_reads, fg_failures);
				last_line = now;
			}
			/* Darwin only from a gauge that is ready (Linux shows 50% before that), and only on a change or twice a minute */
			if (level >= 2 && r.ready && (changed || !reported || report_ns >= 30ULL * NSEC_PER_SEC)) {
				if (!reported) {
					say("fuelgauge: telling Darwin: %d%%, %s\n", r.percent, statuses[r.status]);
				}
				tell_darwin(&r);
				reported = true;
				last_report = now;
			}
			last = r;
			have_last = true;
		}
		delay_for_interval(interval_ms, NSEC_PER_MSEC);
	}
}

void
hakios_pmfg_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("fuelgauge", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (PE_parse_boot_argn("fuelgauge_ms", &interval_ms, sizeof(interval_ms))) {
		interval_ms = interval_ms < 500 ? 500 : interval_ms > 60000 ? 60000 : interval_ms;
	}
	say("fuelgauge: level %u: the PM8150B's fuel gauge and charger, read only, every %u ms%s\n", level, interval_ms,
	    level >= 2 ? "; Darwin is told" : "");
	if (!find_blocks()) {
		return;
	}
	if (kernel_thread_start(fg_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
