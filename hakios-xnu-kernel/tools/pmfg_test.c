/*
 * A test on the Mac of drivers/power/hakios.pmfg.c: the driver's own code (its register accessors renamed by
 * tools/pmfg-test.sh, so that rd()/wr() below play the PMIC arbiter), against registers of a pretend PM8150B.
 * It checks what can be checked without the phone: the arithmetic against Linux's, the states, that only reads
 * are sent and each under the SPMI lock, and what happens when the arbiter or the registers misbehave.
 */

/* what the kernel headers would give hakios.pmfg.c */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
typedef void *thread_t;
typedef int wait_result_t;
typedef int kern_return_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define KERN_SUCCESS 0
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#define KERNEL 1
#define SYSCTL_DECL(x)
#define SYSCTL_INT(...)
#define SYSCTL_QUAD(...)
uint64_t mach_absolute_time(void);
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs);
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns);
void delay_for_interval(uint32_t n, uint64_t scale);
bool ml_get_interrupts_enabled(void);
int PE_parse_boot_argn(const char *name, void *value, int size);
kern_return_t kernel_thread_start(thread_continue_t f, void *p, thread_t *t);
void thread_deallocate(thread_t t);
static uint32_t rd(vm_offset_t address);
static void wr(vm_offset_t address, uint32_t value);

#include <math.h>
#include <stdlib.h>
#include "pmfg_under_test.c"      /* made by tools/pmfg-test.sh */

/* ---- the pretend arbiter and PMIC ---------------------------------------------------------------------- */

static uint8_t pmic[0x10000];           /* slave 2: [peripheral 8 bits][register 8 bits] */
static uint32_t chan_map[ARB_MAX_CHANNELS];
static uint32_t obs_data[ARB_MAX_CHANNELS][2], obs_status[ARB_MAX_CHANNELS];
static uint32_t arb_version = 0x50020000, deny_peripheral = 0;
static int lock_depth, max_lock_depth, transfers;
static int flip_reg = -1, flip_times;           /* a register that reads differently the next flip_times times */
static int writes_elsewhere;
static struct hakios_battery_state told;
static int told_count;
static char logbuf[1 << 16];
static size_t loglen;

vm_offset_t ml_io_map(vm_offset_t phys, __unused vm_size_t size) { return phys; }
void hakios_spmi_lock(void) { lock_depth++; if (lock_depth > max_lock_depth) max_lock_depth = lock_depth; }
void hakios_spmi_unlock(void) { lock_depth--; }
void loader_log_putc(char c) { if (loglen < sizeof(logbuf) - 1) logbuf[loglen++] = c; }
bool ml_get_interrupts_enabled(void) { return false; }
uint64_t mach_absolute_time(void) { static uint64_t t; return t += 1000; }
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs) { *abs = ns; }
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns) { *ns = abs; }
void delay_for_interval(__unused uint32_t n, __unused uint64_t scale) {}
int PE_parse_boot_argn(__unused const char *name, __unused void *value, __unused int size) { return 0; }
kern_return_t kernel_thread_start(__unused thread_continue_t f, __unused void *p, __unused thread_t *t) { return 1; }
void thread_deallocate(__unused thread_t t) {}
void hakios_battery_report(const struct hakios_battery_state *s) { told = *s; told_count++; }

static uint32_t
rd(vm_offset_t a)
{
	if (a >= ARB_CORE_PHYS && a < ARB_CORE_PHYS + ARB_CORE_SIZE) {
		uint32_t off = (uint32_t)(a - ARB_CORE_PHYS);
		if (off == ARB_VERSION) return arb_version;
		if (off == ARB_FEATURES) return ARB_MAX_CHANNELS;
		if (off >= 0x900 && off < 0x900 + 4 * ARB_MAX_CHANNELS) return chan_map[(off - 0x900) / 4];
		return 0;
	}
	if (a >= ARB_OBSERVER_PHYS && a < ARB_OBSERVER_PHYS + ARB_OBSERVER_SIZE) {
		uint32_t ch = (uint32_t)((a - ARB_OBSERVER_PHYS) / 0x80), reg = (uint32_t)((a - ARB_OBSERVER_PHYS) % 0x80);
		if (reg == OBSERVER_STATUS) return obs_status[ch];
		if (reg == OBSERVER_DATA0) return obs_data[ch][0];
		if (reg == OBSERVER_DATA1) return obs_data[ch][1];
		return 0;
	}
	fprintf(stderr, "read of %#lx: not the arbiter\n", (unsigned long)a);
	abort();
}

static void
wr(vm_offset_t a, uint32_t v)
{
	if (a >= ARB_OBSERVER_PHYS && a < ARB_OBSERVER_PHYS + ARB_OBSERVER_SIZE &&
	    (a - ARB_OBSERVER_PHYS) % 0x80 == OBSERVER_COMMAND) {
		uint32_t ch = (uint32_t)((a - ARB_OBSERVER_PHYS) / 0x80), periph = (chan_map[ch] >> 8) & 0xff;
		uint32_t reg = (v >> 4) & 0xff, count = (v & 7) + 1;

		if (lock_depth != 1) { fprintf(stderr, "a transfer outside the SPMI lock\n"); abort(); }
		if ((v & COMMAND_READ) == 0) { fprintf(stderr, "a command that is not a read: %#x\n", v); abort(); }
		transfers++;
		obs_data[ch][0] = obs_data[ch][1] = 0;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t addr = (periph << 8) | ((reg + i) & 0xff);
			uint8_t byte = pmic[addr];
			if ((int)addr == flip_reg && flip_times > 0) { byte ^= 0x5a; flip_times--; }
			obs_data[ch][i / 4] |= (uint32_t)byte << (8 * (i % 4));
		}
		obs_status[ch] = STATUS_DONE | ((chan_map[ch] >> 8) == deny_peripheral ? STATUS_DENIED : 0);
		return;
	}
	writes_elsewhere++;     /* anything but a read command: this driver must never do it */
}

/* ---- a phone as Android left it ------------------------------------------------------------------------ */

static void
set16(uint32_t addr, uint16_t v) { pmic[addr] = v & 0xff; pmic[addr + 1] = v >> 8; }

static void
phone(void)
{
	static const uint32_t periph[] = { 0x240, 0x241, 0x242, 0x243, 0x210, 0x212, 0x213, 0x214, 0x2c0 /* haptics */ };

	memset(pmic, 0, sizeof(pmic));
	memset(chan_map, 0, sizeof(chan_map));
	memset(found, 0, sizeof(found));
	memset(obs_status, 0, sizeof(obs_status));
	deny_peripheral = 0; flip_reg = -1; flip_times = 0; arb_version = 0x50020000;
	/* channels in an order of their own, some of other slaves between them (as on the phone, 512 of them) */
	for (unsigned i = 0; i < sizeof(periph) / sizeof(periph[0]); i++) {
		chan_map[200 + 7 * i] = periph[i] << 8;
	}
	chan_map[3] = 0x040 << 8;       /* slave 0 0x4000: must not be taken for the fuel gauge */
	pmic[0x4004] = 0x0d; pmic[0x4005] = 0x10;       /* BATT_SOC */
	pmic[0x4104] = 0x0d; pmic[0x4105] = 0x11;       /* BATT_INFO */
	pmic[0x4204] = 0x0d; pmic[0x4205] = 0x13;       /* RRADC */
	pmic[0x4010] = SOC_READY_BIT;
}

/* ---- checks -------------------------------------------------------------------------------------------- */

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
set_battery(int raw, int vbatt_uv, int linux_ibatt_ua, int temp_raw)
{
	uint16_t v = (uint16_t)((int64_t)vbatt_uv * 1000 / VBATT_NUMR);
	int16_t i = (int16_t)lround((double)linux_ibatt_ua * 1000 / IBATT_NUMR);

	pmic[0x4009] = pmic[0x400a] = (uint8_t)raw;
	set16(0x41a0, v); set16(0x41a6, v);
	set16(0x41a2, (uint16_t)i); set16(0x41a8, (uint16_t)i);
	set16(0x4288, (uint16_t)temp_raw);
}

static void
set_charger(bool usb, bool dc, int charger)
{
	pmic[0x1310] = usb ? PLUGIN_BIT : 0;
	pmic[0x1410] = dc ? PLUGIN_BIT : 0;
	pmic[0x1006] = (uint8_t)(0xf8 | charger);       /* the bits above [2:0] must not matter */
}

static int
linux_percent(int raw)          /* fg_get_msoc(), worked out in floating point */
{
	if (raw == 255) return 100;
	if (raw == 0) return 0;
	return (int)floor((raw - 1) * 98.0 / 253.0 + 0.5) + 1;
}

int
main(void)
{
	struct reading r;
	const char *problem;

	/* 1. the blocks are found by the arbiter's map, and checked */
	phone();
	CHECK(find_blocks(), "find_blocks() on a phone like the Mi 9");
	CHECK(found[B_SOC] && channels[B_SOC] == 200 && found[B_INFO] && channels[B_INFO] == 207, "SOC/INFO channels %u %u",
	    channels[B_SOC], channels[B_INFO]);
	CHECK(found[B_RRADC] && found[B_CHGR] && found[B_USBIN] && found[B_DCIN], "the optional blocks found");

	/* 2. the TWRP log of 2026-11-02: "battery SOC:100 voltage: 4402576uV temp: 305", off the charger */
	set_battery(255, 4402576, 300000, 122);
	set_charger(false, false, DISABLE_CHARGE);
	problem = read_all(&r);
	CHECK(problem == NULL, "read_all: %s", problem);
	CHECK(r.raw == 255 && r.percent == 100, "100%%: raw %d percent %d", r.raw, r.percent);
	CHECK(abs(r.voltage_uv - 4402576) < 123, "voltage %d uV", r.voltage_uv);
	CHECK(abs(r.current_ua - -300000) < 489, "discharging 300 mA reads %d uA (Darwin: negative)", r.current_ua);
	CHECK(r.temperature_hundredths == TEMPERATURE_UNKNOWN && r.temperature_raw == 122, "temperature %d raw %d",
	    r.temperature_hundredths, r.temperature_raw);
	CHECK(r.ready && !r.missing && !r.usb && !r.dc && r.status == STATUS_DISCHARGING, "status %d", r.status);
	tell_darwin(&r);
	CHECK(told_count == 1 && told.size == sizeof(told) && told.percent_hundredths == 10000 && told.present == 1 &&
	    told.external == 0 && told.status == 2 && told.design_uah == 3300000 && told.charge_uah == 3300000,
	    "Darwin told %d%% present %d external %d status %d design %d charge %d", told.percent_hundredths,
	    told.present, told.external, told.status, told.design_uah, told.charge_uah);
	CHECK((told.percent_hundredths + 50) / 100 == 100, "hakios.battery.c's rounding gives 100");

	/* 3. every raw value: the percentage is Linux's, 0 and 100 only at the ends, never falling as raw rises */
	{
		int previous = -1, bad = 0;
		for (int raw = 0; raw <= 255; raw++) {
			int p = percent_of(raw);
			if (p != linux_percent(raw) || p < previous || (raw > 0 && raw < 255 && (p < 1 || p > 99))) {
				bad++;
				if (bad < 5) printf("  raw %d: %d, Linux %d\n", raw, p, linux_percent(raw));
			}
			previous = p;
		}
		CHECK(bad == 0, "%d raw values differ from fg_get_msoc()", bad);
		CHECK(percent_of(1) == 1 && percent_of(128) == 50 && percent_of(254) == 99, "1 -> %d, 128 -> %d, 254 -> %d",
		    percent_of(1), percent_of(128), percent_of(254));
	}

	/* 4. on a charger, charging at 2 A: Darwin's current is positive */
	set_battery(128, 3900000, -2000000, 100);
	set_charger(true, false, FULLON_CHARGE);
	problem = read_all(&r);
	CHECK(problem == NULL && r.status == STATUS_CHARGING && r.usb && r.percent == 50, "charging: %s status %d pct %d",
	    problem ? problem : "ok", r.status, r.percent);
	CHECK(abs(r.current_ua - 2000000) < 489, "charging 2 A reads %d uA", r.current_ua);
	tell_darwin(&r);
	CHECK(told.external == 1 && told.status == 1 && told.current_ua > 0, "Darwin: external %d status %d current %d",
	    told.external, told.status, told.current_ua);

	/* 5. the wireless pad only, tapering */
	set_charger(false, true, TAPER_CHARGE);
	CHECK(read_all(&r) == NULL && r.dc && !r.usb && r.status == STATUS_CHARGING, "wireless: status %d", r.status);

	/* 6. charging done: 99% or more is 100% and full; at 94% it stays 94% (but full, as Linux says) */
	set_battery(253, 4400000, 0, 100);
	set_charger(true, false, TERMINATE_CHARGE);
	CHECK(read_all(&r) == NULL && r.percent == 100 && r.status == STATUS_FULL, "terminated at raw 253: %d%% status %d",
	    r.percent, r.status);
	set_battery(240, 4300000, 0, 100);
	CHECK(read_all(&r) == NULL && r.percent == percent_of(240) && r.percent == 94 && r.status == STATUS_FULL,
	    "terminated at raw 240: %d%% status %d", r.percent, r.status);
	set_charger(true, false, PAUSE_CHARGE);
	CHECK(read_all(&r) == NULL && r.status == STATUS_NOT_CHARGING, "paused: status %d", r.status);
	set_charger(false, false, PAUSE_CHARGE);
	CHECK(read_all(&r) == NULL && r.status == STATUS_DISCHARGING, "paused, unplugged: status %d", r.status);

	/* 7. the gauge not ready yet, and the battery missing */
	pmic[0x4010] = 0;
	CHECK(read_all(&r) == NULL && !r.ready, "not ready");
	pmic[0x4010] = SOC_READY_BIT;
	pmic[0x4210] = RRADC_BT_MISS_BIT;
	CHECK(read_all(&r) == NULL && r.missing, "missing");
	tell_darwin(&r);
	CHECK(told.present == 0, "Darwin: present %d", told.present);
	pmic[0x4210] = 0;

	/* 8. the temperature: never told to Darwin (its scale is not known); the register's 10 bits kept for the log */
	set_battery(200, 4000000, 100000, 0x19);
	CHECK(read_all(&r) == NULL && r.temperature_hundredths == TEMPERATURE_UNKNOWN && r.temperature_raw == 0x19,
	    "raw 0x19 -> %d (raw %#x)", r.temperature_hundredths, r.temperature_raw);
	tell_darwin(&r);
	CHECK(told.temperature_hundredths == INT32_MIN, "Darwin told %d", told.temperature_hundredths);
	set16(0x4288, 0xfc00 | 0x1a);   /* bits above [9:0] are not the value */
	CHECK(read_all(&r) == NULL && r.temperature_raw == 0x1a, "masked: %#x", r.temperature_raw);

	/* 8b. the plug-in bits settle: one reading that disagrees is not taken (the phone's 104324 run), two are */
	set_battery(255, 4419000, -166000, 0x19);
	set_charger(true, false, TAPER_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb && r.status == STATUS_CHARGING), "first: usb %d status %d", r.usb, r.status);
	set_charger(false, false, TAPER_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb && r.status == STATUS_CHARGING), "one blip: usb %d status %d",
	    r.usb, r.status);
	set_charger(true, false, TAPER_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb && r.status == STATUS_CHARGING), "back: usb %d status %d", r.usb, r.status);
	set_charger(false, false, TAPER_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb), "unplugged, first reading: still usb");
	CHECK(read_all(&r) == NULL && (settle(&r), !r.usb && r.status == STATUS_DISCHARGING), "unplugged, second: usb %d status %d",
	    r.usb, r.status);
	set_battery(253, 4400000, 0, 0x19);
	set_charger(true, false, TERMINATE_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), !r.usb && r.percent == 99 && r.status == STATUS_FULL),
	    "plugged, first reading (terminated): usb %d %d%% status %d", r.usb, r.percent, r.status);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb && r.percent == 100 && r.status == STATUS_FULL),
	    "plugged, second: usb %d %d%% status %d", r.usb, r.percent, r.status);
	set_charger(false, true, FULLON_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb && !r.dc), "usb -> wireless, first: the usb still");
	set_charger(false, false, FULLON_CHARGE);
	CHECK(read_all(&r) == NULL && (settle(&r), r.usb && !r.dc), "a third state before the second agreed: the usb still");
	CHECK(read_all(&r) == NULL && (settle(&r), !r.usb && !r.dc && r.status == STATUS_DISCHARGING), "then nothing: %d %d",
	    r.usb, r.dc);

	/* 9. a value whose copy disagrees: once is read again; always is a failed reading */
	set_battery(180, 3950000, 250000, 100);
	flip_reg = 0x400a; flip_times = 1;
	CHECK(read_all(&r) == NULL && r.raw == 180, "MSOC copy disagreeing once: raw %d", r.raw);
	flip_reg = 0x41a6; flip_times = 100;
	problem = read_all(&r);
	CHECK(problem != NULL && strstr(problem, "copy") != NULL, "VBATT copy always disagreeing: %s", problem ? problem : "accepted");
	flip_reg = 0x400a; flip_times = 100;
	problem = read_all(&r);
	CHECK(problem != NULL, "MSOC copy always disagreeing: accepted");
	flip_times = 0;

	/* 10. the arbiter refusing the gauge: a failed reading, the lock let go */
	deny_peripheral = 0x241;
	problem = read_all(&r);
	CHECK(problem != NULL && strstr(problem, "refuses") != NULL, "denied: %s", problem ? problem : "accepted");
	CHECK(lock_depth == 0, "lock depth after a refusal %d", lock_depth);
	deny_peripheral = 0;

	/* 11. a battery current at the ends of 16 bits, no overflow */
	set16(0x41a2, 0x8000); set16(0x41a8, 0x8000);
	CHECK(read_all(&r) == NULL && r.current_ua == 15999991, "-32768 steps reads %d uA", r.current_ua);
	set16(0x41a2, 0x7fff); set16(0x41a8, 0x7fff);
	CHECK(read_all(&r) == NULL && r.current_ua == -15999503, "32767 steps reads %d uA", r.current_ua);

	/* 12. not the PM8150B's fuel gauge (another subtype), no fuel gauge at all, another arbiter: nothing is done */
	phone();
	pmic[0x4005] = 0x07;
	CHECK(!find_blocks(), "a BATT_SOC of subtype 7 accepted");
	phone();
	chan_map[200] = 0;
	CHECK(!find_blocks(), "no BATT_SOC channel accepted");
	phone();
	arb_version = 0x70000000;
	CHECK(!find_blocks(), "arbiter version 7 accepted");

	/* 13. without the charger's blocks: the charge still read, the state "discharging" */
	phone();
	chan_map[200 + 7 * 4] = chan_map[200 + 7 * 6] = chan_map[200 + 7 * 7] = 0;
	CHECK(find_blocks() && !found[B_CHGR] && !found[B_USBIN], "found without the charger");
	set_battery(100, 3800000, 200000, 100);
	CHECK(read_all(&r) == NULL && r.percent == percent_of(100) && r.status == STATUS_DISCHARGING && r.charger == -1,
	    "without the charger: %d%% status %d", r.percent, r.status);

	CHECK(writes_elsewhere == 0, "%d writes that were not read commands", writes_elsewhere);
	CHECK(max_lock_depth == 1 && lock_depth == 0, "lock depth max %d now %d", max_lock_depth, lock_depth);
	printf("%d checks, %d failed, %d transfers (all reads, each under the SPMI lock)\n", checks, failures, transfers);
	logbuf[loglen] = 0;
	printf("--- the driver's log ---\n%s", logbuf);
	return failures != 0;
}
