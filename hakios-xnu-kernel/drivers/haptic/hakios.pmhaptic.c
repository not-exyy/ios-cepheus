/*
 * The vibration motor of the Xiaomi Mi 9 (cepheus, SM8150): a linear resonant actuator driven by the
 * haptics block of the PM8150B PMIC, Android's "qti-haptics" (drivers/input/misc/qti-haptics.c of
 * Xiaomi's kernel, LineageOS android_kernel_xiaomi_sm8150). It is not the I2C chip of the SM7325 phone
 * (hakios.haptic.c): there is no reset pin, no RAM and no firmware; it is a block of registers on the
 * SPMI bus, reached through the PMIC arbiter the way the keys are (hakios.buttons.c), and it is told
 * either "drive at this voltage until told to stop" (the "VMAX" source, with the chip keeping to the
 * motor's resonance by itself) or "play these eight samples" (the "buffer" source).
 *
 * Every number below is the phone's own, from its device tree (dtbo.img of MIUI V12.5.1.0, the
 * overlay "CEPHEUS", qcom,pm8150b@3 qcom,haptics@c000) and the Linux driver's register map:
 *
 *   the block  slave 3 of the PM8150B, peripheral 0xc0 (registers 0x3c000..0x3c0ff), its channel of the
 *              arbiter the one this system (0) owns
 *   the motor  LRA, vmax 2800 mV, a resonance period of 5102 us (196 Hz), a sine drive, auto-resonance
 *              by "QWD"; Xiaomi's build has the calibration's end-of-pattern on, its period 0
 *   effects    Android's six (wf_0..wf_5: click, double click, tick, thud, pop, heavy click), each
 *              2-3 samples of the buffer at 3600 mV with a brake pattern, auto-resonance off
 *
 * Nothing here is done unless a boot-arg asks for it; the levels are hakios.haptic.c's:
 *
 *   haptic=1   a look, reading only: the arbiter's channel of the block, who owns it, its type and
 *              registers as Android's boot loader leaves them (the status, the short-circuit flag)
 *   haptic=2   the same (there is no reset to release here)
 *   haptic=3   ... and Android's configuration written (qti_haptics_hw_init(), every write read back);
 *              the block stays disabled. Nothing moves.
 *   haptic=4   ... and one of Android's effects played (haptic_effect=0..5, default 0: the click), or with
 *              haptic_ms=N a vibration of N ms at Android's 2800 mV (at most HAPTIC_MAX_MS)
 *   haptic=5   ... and the motor for Darwin until the run ends: what Darwin plays on its "Actuator" device
 *              (userland/src/audio_plugin.c) comes to /dev/hakios-haptic as samples of one channel, as
 *              with hakios.haptic.c; each waveform of Darwin's (a stretch of samples after a pause) is a
 *              tap -- Android's effect haptic_tap= (default 0) -- and one that goes on for longer than a
 *              tap a vibration from the VMAX source until it ends, its voltage following how loud
 *              Darwin's waveform is, for HAPTIC_MAX_MS at most (then the motor rests as long)
 *
 * Limits, whatever asks: Android's voltages only (3600 mV for its effects, 2800 for a vibration, five
 * eighths of those for the softest of Darwin's), never more; no vibration longer than HAPTIC_MAX_MS; the
 * block's short-circuit flag looked at every 50 ms while it plays, and a short circuit, a transfer the
 * arbiter does not complete or a write that does not read back stops it (PLAY off, the block disabled)
 * and nothing more is done for the rest of the run. Nothing is written but the registers of this one
 * block, and none of its "secure" ones (above 0xd0). At the run's end, and on a panic's way out, the
 * block is told to stop and is disabled.
 *
 * Checked: compiled. On the phone (2026-11-04, logs/145903-run.log, haptic=1, iOS running beside it): arbiter
 * version 5, 512 channels; the block is channel 307 (0x103c000), written by system 0 -- ours; type 0x15 subtype 0x09;
 * status1 0, no short circuit. The boot loader has already configured it much as Android does: EN_CTL1 0 (off),
 * EN_CTL2 0x03, EN_CTL3 0xff, AUTO_RES_CTRL 0xdc, CFG1/CFG2 0 (LRA, sine), SEL 0, AUTO_RES_CFG 0x90, VMAX 0x30
 * (2784 mV), ILIM 0x01, RATE 0x3ef (5035 us), ZX_CFG 3, the buffer empty, PLAY 0.
 * haptic=3 (logs/150208): every write read back -- ILIM keeps only bit 0 and a sample only bits 7:1 (Linux's 0x02 and
 * 0x01 read back 0), so those are checked by that. haptic=4 (150420): the click played and cleared; haptic_ms=300
 * (150513): status1 0x82 (busy) while it vibrates, the block off after. haptic=5 (150609, 120 s with the user touching
 * iOS): 83 taps and 16 vibrations, none cut, no fault; felt on the phone and called "perfect" by the user.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

static uint32_t level;
static volatile bool broken, leaving;
static bool configured, enabled;

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
	if (!leaving && ml_get_interrupts_enabled()) {
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

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_us(uint32_t us)
{
	if (leaving || us < 1000) {
		spin_us(us);
	} else {
		delay_for_interval(us, NSEC_PER_USEC);
	}
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- the PMIC arbiter (version 5), as hakios.buttons.c has it ------------------------------------------ */

#define ARB_CORE_PHYS           0x0c440000UL    /* "core": the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* "cnfg": which system owns each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* "obsrvr": 0x10000 per system, 0x80 per channel in it */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHANNELS_PHYS       0x0c600000UL    /* "chnls": 0x10000 per channel, for its owner's writes */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long" */
#define TRANSFER_TIMEOUT_US     2000

#define HAP_PERIPHERAL          0x3c0           /* slave 3, peripheral 0xc0: the device tree's pm8150b@3 haptics@c000 */

static vm_offset_t arb_core, arb_cnfg, arb_observer, arb_channel;
static uint32_t channel = ~0u, owner = ~0u;

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

/* Read `count' (1 to 8) registers of the block from `reg' on. Returns NULL, or what went wrong. */
static const char *
spmi_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t c = arb_observer + 0x80UL * channel;
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status, data[2];
	const char *problem;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	if (!leaving) {
		hakios_spmi_lock();
	}
	wr(c + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(c + OBSERVER_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	data[0] = rd(c + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(c + OBSERVER_DATA1) : 0;
	if (!leaving) {
		hakios_spmi_unlock();
	}
	if ((problem = status_problem(status)) == NULL) {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	return problem;
}

/* Write one register of the block through its channel, which is this system's (checked once, in find_block()). */
static const char *
spmi_write(uint8_t reg, uint8_t byte)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	if (arb_channel == 0) {
		return "the block's channel is not this system's to write through";
	}
	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	if (!leaving) {
		hakios_spmi_lock();
	}
	wr(arb_channel + CHANNEL_WDATA0, byte);
	wr(arb_channel + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4 | 0);
	do {
		status = rd(arb_channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	if (!leaving) {
		hakios_spmi_unlock();
	}
	return status_problem(status);
}

/* The arbiter, and the block's channel: the first, unless a later one is ours to write through (as Linux chooses). */
static bool
find_block(void)
{
	uint32_t version, channels;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS + 0x10000UL * ARB_SYSTEM, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	say("haptic: the PMIC arbiter is version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("haptic: not the arbiter this driver knows (version 5): nothing is done\n");
		return false;
	}
	if (channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), o;

		if (map == 0 || ((map >> 8) & 0xfff) != HAP_PERIPHERAL) {
			continue;
		}
		o = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("haptic: channel %u is the PM8150B's haptics (%#x); written by system %u\n", n, map, o);
		if (channel == ~0u || o == ARB_SYSTEM) {
			channel = n;
			owner = o;
		}
	}
	if (channel == ~0u) {
		say("haptic: no channel of the arbiter is the PM8150B's haptics (slave 3, peripheral 0xc0): nothing is done\n");
		return false;
	}
	if (owner == ARB_SYSTEM) {
		arb_channel = ml_io_map(ARB_CHANNELS_PHYS + 0x10000UL * channel, 0x1000);
	}
	return true;
}

/* ---- the haptics block: Linux's qti-haptics.c ---------------------------------------------------------- */

#define REG_TYPE                0x04
#define REG_SUBTYPE             0x05
#define REG_HAP_STATUS1         0x0a
#define HAP_SC_DET              0x08
#define HAP_BUSY                0x02
#define REG_HAP_INT_RT_STS      0x10            /* [0] short circuit, [1] play */
#define REG_HAP_EN_CTL1         0x46            /* [7] the block on */
#define REG_HAP_EN_CTL2         0x48            /* [1] auto standby, [0] brake */
#define REG_HAP_EN_CTL3         0x4a
#define REG_HAP_AUTO_RES_CTRL   0x4b            /* [7] auto-resonance on */
#define REG_HAP_CFG1            0x4c            /* the actuator: 0 LRA */
#define REG_HAP_CFG2            0x4d            /* the drive: 0 sine */
#define REG_HAP_SEL             0x4e            /* [5:4] the source: 0 VMAX, 1 the buffer; [0] external trigger */
#define REG_HAP_AUTO_RES_CFG    0x4f
#define REG_HAP_VMAX_CFG        0x51            /* [5:1] the voltage, 116 mV a step */
#define REG_HAP_ILIM_CFG        0x52
#define REG_HAP_RATE_CFG1       0x54            /* the play rate in 5 us: low byte */
#define REG_HAP_RATE_CFG2       0x55            /* ... [3:0] its high bits */
#define REG_HAP_ZX_CFG          0x5a            /* [2:0] the zero-crossing debounce */
#define REG_HAP_BRAKE           0x5c            /* 2 bits per brake sample */
#define REG_HAP_WF_REPEAT       0x5e            /* [6:4] the buffer's repeats (2^n), [1:0] each sample's (2^n) */
#define REG_HAP_WF_S1           0x60            /* .. 0x67: the buffer's samples, [5:1] amplitude, [6] overdrive */
#define WF_BITS                 0xfe            /* of a sample: bit 0 is not kept (Linux's clear writes 0x01, 0x00 reads back) */
#define REG_HAP_PLAY            0x70            /* [7] play */

#define EN_CTL3_ANDROID         0xfd            /* H-bridge, PWM signal, ILIM, ILIM CC, auto-res bias, DAC, PWM ctl */
#define AUTO_RES_CTRL_ANDROID   0xec            /* on, by the period, error delta 2, recovery, delay 4 */
#define AUTO_RES_CFG_QWD        0x80
#define AUTO_RES_CFG_CAL_EOP    0x08
#define VMAX_MV_LSB             116
#define VMAX_MV_MAX             3596
#define PLAY_RATE_US            5102            /* qcom,play-rate-us */
#define CONFIG_VMAX_MV          2800            /* qcom,vmax-mv */
#define HAPTIC_MAX_MS           1000

/* Android's effects, from the device tree (qcom,wf-*), with the brake pattern as verify_brake_setting() packs it */
static const struct effect {
	const char *    name;
	uint8_t         pattern[3], length;
	uint8_t         repeat_n, s_repeat_n;   /* REG_HAP_WF_REPEAT's fields: 2^n times */
	uint8_t         brake, brake_length;
} effects[] = {
	{ "click",        { 0x3e, 0x3e },       2, 0, 0, 0x06, 2 },     /* brake <2 1 0 0> */
	{ "double click", { 0x7e, 0x3e },       2, 1, 0, 0x17, 3 },     /* brake <3 1 1 0>, wf-repeat-count 2 */
	{ "tick",         { 0x7e, 0x3e },       2, 0, 0, 0x17, 3 },
	{ "thud",         { 0x3e, 0x3e },       2, 0, 0, 0x06, 2 },
	{ "pop",          { 0x3e, 0x3e },       2, 0, 0, 0x06, 2 },
	{ "heavy click",  { 0x7e, 0x7e, 0x7e }, 3, 0, 0, 0x1f, 3 },     /* brake <3 3 1 0> */
};
#define EFFECT_VMAX_MV          3600            /* qcom,wf-vmax-mv of every one */

static void
stop_now(void)
{
	(void)spmi_write(REG_HAP_PLAY, 0x00);
	(void)spmi_write(REG_HAP_EN_CTL1, 0x00);
	enabled = false;
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		stop_now();
		say("haptic: GIVEN UP: %s; the block is told to stop and disabled, and nothing more is done\n", why);
	}
}

static bool
get(uint8_t reg, uint8_t *value)
{
	const char *problem;

	if ((problem = spmi_read(reg, value, 1)) != NULL) {
		say("haptic: reading register %#04x: %s\n", reg, problem);
		give_up("a read failed");
		return false;
	}
	return true;
}

/* Write a register and, unless `check' is 0, read it back: what `check' covers must have been taken. */
static bool
put_checked(uint8_t reg, uint8_t value, uint8_t check)
{
	const char *problem;
	uint8_t back = 0;

	if (broken) {
		return false;
	}
	if ((problem = spmi_write(reg, value)) != NULL) {
		say("haptic: writing %#04x to register %#04x: %s\n", value, reg, problem);
		give_up("a write failed");
		return false;
	}
	if (check != 0) {
		if ((problem = spmi_read(reg, &back, 1)) != NULL || ((back ^ value) & check) != 0) {
			say("haptic: register %#04x: %#04x written, %s %#04x\n", reg, value, problem ? problem : "read back", back);
			give_up("a write did not read back");
			return false;
		}
	}
	return true;
}

static bool
put(uint8_t reg, uint8_t value)
{
	return put_checked(reg, value, 0xff);
}

/* the bits of `mask' to `value', the others as they are (regmap_update_bits) */
static bool
update(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	return get(reg, &now) && put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

static void
show_registers(const char *when)
{
	uint8_t id[2] = { 0, 0 }, st[1] = { 0 }, rt[1] = { 0 }, a[8], b[8], c[8], s[8], play = 0;
	const char *problem;

	if ((problem = spmi_read(REG_TYPE, id, 2)) != NULL ||
	    (problem = spmi_read(REG_HAP_STATUS1, st, 1)) != NULL ||
	    (problem = spmi_read(REG_HAP_INT_RT_STS, rt, 1)) != NULL ||
	    (problem = spmi_read(0x46, a, 8)) != NULL ||
	    (problem = spmi_read(0x4e, b, 8)) != NULL ||
	    (problem = spmi_read(0x56, c, 8)) != NULL ||
	    (problem = spmi_read(REG_HAP_WF_S1, s, 8)) != NULL ||
	    (problem = spmi_read(REG_HAP_PLAY, &play, 1)) != NULL) {
		say("haptic: the block's registers (%s) cannot be read: %s\n", when, problem);
		give_up("its registers cannot be read");
		return;
	}
	say("haptic: registers %s: type %#04x subtype %#04x, status1 %#04x%s%s, rt_sts %#04x\n", when, id[0], id[1], st[0],
	    (st[0] & HAP_SC_DET) ? " (SHORT CIRCUIT)" : "", (st[0] & HAP_BUSY) ? " (busy)" : "", rt[0]);
	say("haptic:   0x46: %02x %02x %02x %02x %02x %02x %02x %02x  0x4e: %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	say("haptic:   0x56: %02x %02x %02x %02x %02x %02x %02x %02x  0x60: %02x %02x %02x %02x %02x %02x %02x %02x  play %02x\n",
	    c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], play);
}

/* qti_haptics_hw_init() with the phone's values: the block stays disabled */
static bool
configure(void)
{
	uint16_t rate = PLAY_RATE_US / 5;

	say("haptic: Android's configuration is written (LRA, %u us, sine, auto-resonance QWD), each write read back\n",
	    PLAY_RATE_US);
	configured = put(REG_HAP_EN_CTL1, 0x00) && put_checked(REG_HAP_PLAY, 0x00, 0) &&
	    put(REG_HAP_CFG1, 0x00) &&                          /* LRA */
	    /*
	     * Linux's "1000 mA" (bit 1); this block keeps only bit 0, and reads 0x00 back after it (logs/150100-run.log):
	     * what Android runs with, the lower limit. The boot loader leaves 0x01.
	     */
	    put_checked(REG_HAP_ILIM_CFG, 0x02, 0x01) &&
	    put(REG_HAP_EN_CTL3, EN_CTL3_ANDROID) &&
	    update(REG_HAP_ZX_CFG, 0x07, 0x03) &&               /* 80 us */
	    put(REG_HAP_RATE_CFG1, rate & 0xff) && put_checked(REG_HAP_RATE_CFG2, (rate >> 8) & 0x0f, 0x0f) &&
	    put(REG_HAP_CFG2, 0x00) &&                          /* sine */
	    update(REG_HAP_AUTO_RES_CFG, 0x8f, AUTO_RES_CFG_QWD | AUTO_RES_CFG_CAL_EOP) &&
	    put(REG_HAP_AUTO_RES_CTRL, AUTO_RES_CTRL_ANDROID);
	if (configured) {
		show_registers("as configured");
	}
	return configured && !broken;
}

static uint8_t
vmax_field(uint32_t mv)
{
	mv = mv > VMAX_MV_MAX ? VMAX_MV_MAX : mv;
	return (uint8_t)((mv / VMAX_MV_LSB) << 1);
}

/* Android's effect `e' loaded (qti_haptics_load_predefined_effect()) and played; returns how long it lasts, 0 if not */
static uint32_t
start_effect(const struct effect *e, uint32_t vmax_mv)
{
	uint32_t us = (uint32_t)e->length * PLAY_RATE_US * (1u << e->repeat_n) * (1u << e->s_repeat_n) +
	    (uint32_t)e->brake_length * PLAY_RATE_US;
	uint16_t rate = PLAY_RATE_US / 5;
	bool ok;

	ok = update(REG_HAP_VMAX_CFG, 0x3e, vmax_field(vmax_mv)) &&
	    put(REG_HAP_RATE_CFG1, rate & 0xff) && put_checked(REG_HAP_RATE_CFG2, (rate >> 8) & 0x0f, 0x0f) &&
	    update(REG_HAP_AUTO_RES_CTRL, 0x80, 0x00) &&        /* qcom,lra-auto-resonance-disable */
	    put(REG_HAP_BRAKE, e->brake) && update(REG_HAP_EN_CTL2, 0x01, 0x01);
	for (unsigned i = 0; ok && i < 8; i++) {
		ok = put_checked(REG_HAP_WF_S1 + i, i < e->length ? e->pattern[i] : 0x00, WF_BITS);
	}
	ok = ok && update(REG_HAP_WF_REPEAT, 0x73, (uint8_t)(e->repeat_n << 4 | e->s_repeat_n)) &&
	    update(REG_HAP_SEL, 0x31, 0x10) &&                  /* the buffer */
	    put(REG_HAP_EN_CTL1, 0x80) && put_checked(REG_HAP_PLAY, 0x80, 0);
	enabled = ok || enabled;
	return ok ? us : 0;
}

/* a vibration from the VMAX source (qti_haptics_load_constant_waveform(), 20 ms or more): until stopped */
static bool
start_vibration(uint32_t vmax_mv)
{
	uint16_t rate = PLAY_RATE_US / 5;
	bool ok;

	ok = put(REG_HAP_RATE_CFG1, rate & 0xff) && put_checked(REG_HAP_RATE_CFG2, (rate >> 8) & 0x0f, 0x0f) &&
	    update(REG_HAP_VMAX_CFG, 0x3e, vmax_field(vmax_mv)) &&
	    update(REG_HAP_AUTO_RES_CTRL, 0x80, 0x80) &&        /* auto-resonance, for an LRA from VMAX */
	    update(REG_HAP_SEL, 0x31, 0x00) &&                  /* VMAX */
	    put(REG_HAP_EN_CTL1, 0x80) && put_checked(REG_HAP_PLAY, 0x80, 0);
	enabled = ok || enabled;
	return ok;
}

/*
 * The end of whatever plays: PLAY off, then what Android's erase does (qti_haptics_clear_settings(): brake and
 * auto-resonance off, one sample at 15 us played and stopped), and the block disabled a period and a millisecond later.
 */
static bool
stop_playing(void)
{
	bool ok;

	ok = put_checked(REG_HAP_PLAY, 0x00, 0) &&
	    update(REG_HAP_EN_CTL2, 0x01, 0x00) && update(REG_HAP_AUTO_RES_CTRL, 0x80, 0x00) &&
	    put(REG_HAP_RATE_CFG1, 15 / 5) && put_checked(REG_HAP_RATE_CFG2, 0x00, 0x0f) &&
	    put_checked(REG_HAP_WF_S1, 0x01, WF_BITS);
	for (unsigned i = 1; ok && i < 8; i++) {
		ok = put_checked(REG_HAP_WF_S1 + i, 0x00, WF_BITS);
	}
	ok = ok && put_checked(REG_HAP_PLAY, 0x80, 0) && put_checked(REG_HAP_PLAY, 0x00, 0);
	wait_us(PLAY_RATE_US + 1000);
	ok = ok && put(REG_HAP_EN_CTL1, 0x00);
	enabled = !ok && enabled;
	if (!ok) {
		give_up("it does not stop");
	}
	return ok;
}

/* the short-circuit flag, while it plays */
static bool
healthy(const char *when)
{
	uint8_t status = 0;

	if (!get(REG_HAP_STATUS1, &status)) {
		return false;
	}
	if (status & HAP_SC_DET) {
		say("haptic: %s: status1 %#04x: the block reports a short circuit\n", when, status);
		give_up("a short circuit");
		return false;
	}
	return true;
}

/* play for `us' (an effect's length, or a vibration's), looking at the block while it does */
static void
play_for(uint32_t us)
{
	uint64_t until = now_us() + us;

	while (!broken && now_us() < until) {
		wait_us(us < 20000 ? us : 20000);
		(void)healthy("while playing");
	}
}

/* ---- haptic=5: the motor for Darwin (the way hakios.haptic.c listens to it) ------------------------------ */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long Darwin's waveform is listened to before a tap: its loudness */
#define FOLLOW_US               20000           /* how often a vibration's strength follows Darwin's */
#define TOLD_EVENTS             24              /* the first so many taps and vibrations are each said */

static volatile uint64_t heard_us, quiet_us, onset_us;
static volatile uint32_t heard_peak;
static uint64_t written_frames;
static unsigned taps, loops, capped;

static void
heard(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
		onset_us = now;
		heard_peak = 0;
	}
	heard_us = now;
	if (peak > heard_peak) {
		heard_peak = peak;
	}
}

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		heard(peak);
	}
	return error;
}

/* hakios.mic.c's stand-in for Darwin's waveform (mic_tap=1), as in hakios.haptic.c */
void
hakios_haptic_pretend(uint32_t peak)
{
	heard(peak);
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* Android's voltage `top' from a third of full scale up, down to five eighths of it for the softest; never above */
static uint32_t
mv_for(uint32_t peak, uint32_t top)
{
	uint32_t low = top * 5 / 8, third = 32767 / 3;

	return peak >= third ? top : low + (top - low) * peak / third;
}

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, tap_ends = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0;
	uint32_t tap = 0, playing_mv = 0;
	unsigned follows = 0;
	bool effect_on = false;                 /* a tap's effect is loaded and PLAY is on */
	int major = cdevsw_add(-1, &haptic_cdevsw);

	PE_parse_boot_argn("haptic_tap", &tap, sizeof(tap));
	tap = tap < sizeof(effects) / sizeof(effects[0]) ? tap : 0;
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT DARWIN PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends; "
	    "a tap is Android's %s\n", effects[tap].name);
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;

		switch (state) {
		case IDLE:
		case TAP:
			if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
				uint64_t began = onset_us;
				uint32_t mv = mv_for(peak, EFFECT_VMAX_MV), us;

				if (effect_on && !stop_playing()) {
					break;
				}
				effect_on = false;
				if ((us = start_effect(&effects[tap], mv)) == 0) {
					break;
				}
				state = TAP;
				effect_on = true;
				since = now;
				tap_ends = now + us;
				tap_onset = began;
				taps++;
				if (taps + loops <= TOLD_EVENTS) {
					say("haptic: tap %u: Darwin's waveform peaks at %u of 32768: Android's %s at %u mV, %u us\n",
					    taps, peak, effects[tap].name, mv, us);
				}
				break;
			}
			if (state == IDLE) {
				break;
			}
			/*
			 * The effect is over when its samples and its brake are: PLAY off then (Linux does it from the block's
			 * "play" interrupt at the buffer's end; with PLAY left on, the buffer would be played again).
			 */
			if (effect_on && now >= tap_ends) {
				if (!stop_playing()) {
					break;
				}
				effect_on = false;
			}
			if (active && quiet_us < tap_onset && now - since > TAP_US) {
				uint32_t mv = mv_for(peak, CONFIG_VMAX_MV);

				if ((!effect_on || stop_playing()) && start_vibration(mv)) {
					effect_on = false;
					state = LOOP;
					since = followed = now;
					playing_mv = mv;
					follows = 0;
					loops++;
					heard_peak = 0;
					if (taps + loops <= TOLD_EVENTS) {
						say("haptic: vibration %u: Darwin goes on playing (peak %u): the VMAX source at %u mV, %u ms at most\n",
						    loops, peak, mv, HAPTIC_MAX_MS);
					}
				}
			} else if (!effect_on && (!active || now - since > TAP_US)) {
				state = IDLE;
			}
			break;
		case LOOP:
			if (active && now - followed >= FOLLOW_US) {
				uint32_t mv = mv_for(peak, CONFIG_VMAX_MV);

				followed = now;
				heard_peak = 0;
				if (peak != 0 && (mv > playing_mv + VMAX_MV_LSB || mv + VMAX_MV_LSB < playing_mv)) {
					if (!update(REG_HAP_VMAX_CFG, 0x3e, vmax_field(mv))) {
						break;
					}
					playing_mv = mv;
					follows++;
				}
			}
			if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
				bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				if (stop_playing()) {
					state = rest_until > now ? REST : IDLE;
					if (taps + loops <= TOLD_EVENTS + 1) {
						say("haptic: vibration %u: %s after %llu ms (its voltage changed %u times, last to %u mV); "
						    "the block is disabled\n", loops, cut ? "CUT at the limit" : "Darwin stopped, and so does the motor",
						    (now - since) / 1000, follows, playing_mv);
					}
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (state != IDLE && state != REST && now - last_look > 50000) {
			last_look = now;
			if (!healthy("while playing")) {
				break;
			}
		}
		wait_us(4000);
	}
	say("haptic: the motor is given up on (%u taps and %u vibrations played, %u cut at %u ms)\n", taps, loops, capped,
	    HAPTIC_MAX_MS);
}

/* ---- the levels ------------------------------------------------------------------------------------------ */

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("haptic: level %u (the PM8150B's haptics block): %s\n", level, level <= 2 ? "a look, reading only" :
	    level == 3 ? "configured, nothing played" : level == 4 ? "one effect played" : "the motor for Darwin");
	if (!find_block()) {
		goto done;
	}
	show_registers("as the boot loader leaves them");
	if (broken || level <= 2) {
		goto done;
	}
	if (owner != ARB_SYSTEM) {
		say("haptic: the block's channel is system %u's, not this one's: nothing is written\n", owner);
		goto done;
	}
	if (!configure()) {
		goto done;
	}
	if (level == 4) {
		uint32_t effect = 0, ms = 0;

		PE_parse_boot_argn("haptic_effect", &effect, sizeof(effect));
		PE_parse_boot_argn("haptic_ms", &ms, sizeof(ms));
		effect = effect < sizeof(effects) / sizeof(effects[0]) ? effect : 0;
		if (ms != 0) {
			ms = ms < 20 ? 20 : ms > HAPTIC_MAX_MS ? HAPTIC_MAX_MS : ms;
			say("haptic: a vibration of %u ms at %u mV (the VMAX source)\n", ms, CONFIG_VMAX_MV);
			if (start_vibration(CONFIG_VMAX_MV)) {
				play_for(ms * 1000);
				show_registers("while it vibrates");
				(void)stop_playing();
			}
		} else {
			uint32_t us;

			say("haptic: Android's effect %u (%s) at %u mV\n", effect, effects[effect].name, EFFECT_VMAX_MV);
			if ((us = start_effect(&effects[effect], EFFECT_VMAX_MV)) != 0) {
				play_for(us);
				(void)stop_playing();
			}
		}
		if (!broken) {
			show_registers("after it");
		}
	} else if (level == 5) {
		serve();
	}
done:
	say("haptic: done\n");
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 5) {
		level = 5;
	}
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: PLAY off and the block disabled, if it was ever configured. */
void
hakios_haptic_quiesce(void)
{
	if (configured && arb_channel != 0) {
		leaving = true;
		stop_now();
		if (level == 5) {
			say("haptic: at the run's end: %u taps and %u vibrations played (%u cut at %u ms), from %llu frames Darwin "
			    "wrote; the block is disabled\n", taps, loops, capped, HAPTIC_MAX_MS, written_frames);
		}
	}
}
