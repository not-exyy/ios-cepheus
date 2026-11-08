/*
 * The brightness of the Xiaomi Mi 9's screen (cepheus, SM8150), and the slider of iOS that sets it.
 *
 * The panel. The phone's device tree (devices/cepheus/prebuilt/device-tree.dtb, the panel the boot loader picked:
 * qcom,mdss_dsi_samsung_fhd_ea8076_f1_cmd, the same in its f1p2 and f1mp variants) has a Samsung EA8076 AMOLED in DSI
 * command mode with
 *
 *   qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs"      the brightness is a DCS command to the panel, not the
 *                                                           PM8150L's WLED (that one is for the LCD panels of the tree)
 *   qcom,mdss-dsi-bl-dcs-type-ss                            ... its two bytes the high one first (Linux's
 *                                                           mipi_dsi_dcs_set_display_brightness_ss())
 *   qcom,mdss-dsi-bl-min-level = 2, bl-max-level = 0x7ff    the levels the panel is given, 2..2047 (above is "HBM",
 *                                                           0x53 0xe8, which is not used here)
 *   qcom,mdss-dsi-dma-trigger = "trigger_sw"                commands are sent when software says so
 *
 * so one level is the DSI packet "DCS long write" (data type 0x39) of three bytes, 0x51 (SET_DISPLAY_BRIGHTNESS),
 * level >> 8, level & 0xff. Xiaomi's kernel (LineageOS android_kernel_xiaomi_sm8150, drivers/gpu/drm/msm/dsi-staging)
 * sends such a packet through DSI controller 0 (0x0ae94000, "qcom,dsi-ctrl-hw-v2.3"); of its two ways this driver
 * takes the one that needs no memory the controller reads (so nothing of the display's SMMU): the command DMA's FIFO,
 * which dsi_ctrl_hw_cmn_kickoff_fifo_command() fills through the test pattern generator's registers --
 *
 *   TEST_PATTERN_GEN_CTRL (0x15c) = CMD_DMA_TPG_EN | TPG_DMA_FIFO_MODE | CMD_DMA_PATTERN_SEL custom (bits 1, 2, 17:16)
 *   TEST_PATTERN_GEN_CMD_DMA_INIT_VAL (0x17c) <- the packet, a word at a time (an even number of words)
 *   COMMAND_MODE_DMA_CTRL (0x3c): EMBEDDED_MODE (bit 28), not broadcast (31, 30), high speed (26 clear)
 *   DMA_CMD_LENGTH (0x4c) = its bytes, then CMD_MODE_DMA_SW_TRIGGER (0x90) = 1
 *   ... INT_CTRL (0x110) bit 0, CMD_MODE_DMA_DONE, is waited for and cleared; then dsi_ctrl_hw_cmn_reset_cmd_fifo():
 *   TEST_PATTERN_GEN_CTRL = 0, TPG_DMA_FIFO_RESET (0x1ec) 1 then 0
 *
 * The packet in the FIFO is as dsi_ctrl_copy_and_pad_cmd() lays it out: the header's bytes word count (2), data
 * type, then flags (bit 6 "long packet", bit 7 "last command"; the controller adds the ECC), then the payload padded
 * with 0xff to a word. The boot loader leaves the controller sending the framebuffer to the panel by itself at every
 * tearing-effect signal (the "autorefresh" MiniIOMobileFramebuffer's fb_pp=1 found); a command is triggered just
 * after one such frame has been sent (DSI_STATUS bit 2, CMD_MODE_MDP_BUSY, falling), when the link has some 3 ms to
 * spare before the next.
 *
 * iOS. CoreBrightness (in backboardd) takes for the built-in display's backlight the service that has the property
 * "backlight-control" = true (IOServiceGetMatchingServices with IOPropertyMatch) and checks that it is an
 * "AppleARMBacklight"; it reads the range from its "IODisplayParameters" = { brightness = { min, max, value } } and
 * sets a brightness with IORegistryEntrySetCFProperties(service, { brightness = n, FadePeriod = ..., backlight-level
 * = ... }) (found in the 20F66 shared cache: CoreBrightness's Display.m keeps the three keys in one table). That
 * service is drivers/gpu/hakios.backlight.cpp; what it is given comes here (hakios_backlight_request()): brightness
 * is the panel's level, 0..2047, faded to over FadePeriod.
 *
 * Boot-args (nothing is done without backlight=):
 *
 *   backlight=1        a look, reading only: DSI controller 0's version and the registers a command goes through
 *   backlight=2        ... and the service for iOS: the slider in Control Center and in Settings sets the panel
 *   backlight_test=N   with 1 or 2: once, a few seconds after the start, the panel set to level N (2..2047), then
 *                      after 3 s back to backlight_default; a test of the command without iOS
 *   backlight_default=N  the level iOS is told the panel is at when the service is made (default 1024), and the one
 *                      backlight_test= returns to
 *   backlight_off=1    brightness 0 makes the panel dark (by default the least is the panel's own least, 2: with
 *                      the screen "off" it still shows, dimly)
 *   backlight_lp=1     the command in low-power mode (the panel's own commands in its tree are; the brightness of
 *                      Linux's is sent at high speed, the default here)
 *
 * Limits: levels 0 (only with backlight_off=1) or 2..2047, never the panel's HBM; nothing is written unless DSI 0 is
 * on, in command mode, with software as its command trigger and no command of the boot loader's left in flight; a
 * command the controller does not finish in 50 ms, or one it reports an error for, ends the driver's writes for the
 * rest of the run. Only DSI 0's command-DMA registers are written, and what they held before is put back after
 * every command.
 *
 * Checked on the phone (2026-11-08, logs/200553-run.log, backlight=2 backlight_test=300): DSI 0 version 0x20030000,
 * ctrl 0x1f5, trigger 4 (software), dma_ctrl 0x14000000 as the boot loader leaves it; every command done at the first
 * look at INT_CTRL, ACK_ERR_STATUS 0 throughout. The test's 300 and 1024 seen on the panel; then CoreBrightness's
 * own ramps (a request a frame: FadePeriod=0 brightness=N backlight-level=N) as the Control Center slider was moved,
 * the panel following, confirmed by the user.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void IOSleep(unsigned int milliseconds);

/* hakios.backlight.cpp: the service iOS finds */
extern int HakiosBacklightPublish(uint32_t min, uint32_t max, uint32_t value);

#define DSI0_PHYS                       0x0ae94000UL    /* dt: qcom,mdss_dsi_ctrl0@ae94000, reg 0xae94000 0x400 */

#define DSI_HW_VERSION                  0x000
#define DSI_CTRL                        0x004           /* bit 0 the controller, bit 2 command mode */
#define DSI_STATUS                      0x008           /* bit 2 CMD_MODE_MDP_BUSY, bit 3 CMD_MODE_DMA_BUSY */
#define DSI_FIFO_STATUS                 0x00c
#define DSI_COMMAND_MODE_DMA_CTRL       0x03c
#define DSI_DMA_CMD_LENGTH              0x04c
#define DSI_DMA_FIFO_CTRL               0x050
#define DSI_ACK_ERR_STATUS              0x068
#define DSI_TRIG_CTRL                   0x084           /* bits 2:0 the DMA trigger: 4 = software */
#define DSI_CMD_MODE_DMA_SW_TRIGGER     0x090
#define DSI_TIMEOUT_STATUS              0x0c0
#define DSI_INT_CTRL                    0x110           /* bit 0 CMD_MODE_DMA_DONE; the odd bits are enables */
#define DSI_TEST_PATTERN_GEN_CTRL       0x15c
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x17c
#define DSI_TPG_DMA_FIFO_STATUS         0x1dc
#define DSI_TPG_DMA_FIFO_RESET          0x1ec

#define STATUS_MDP_BUSY                 (1u << 2)
#define STATUS_DMA_BUSY                 (1u << 3)
#define INT_DMA_DONE                    (1u << 0)
#define INT_ENABLES                     0xaaaaaaaau     /* written back as they were; an even (status) bit 1 clears it */
#define DMA_CTRL_BROADCAST              (3u << 30)
#define DMA_CTRL_EMBEDDED               (1u << 28)
#define DMA_CTRL_LOW_POWER              (1u << 26)
#define TPG_FIFO_CUSTOM                 ((1u << 1) | (1u << 2) | (3u << 16))
#define TRIGGER_SW                      4

#define DCS_LONG_WRITE                  0x39
#define DCS_SET_DISPLAY_BRIGHTNESS      0x51
#define HEADER_LONG                     (1u << 6)
#define HEADER_LAST                     (1u << 7)

#define LEVEL_MIN                       2               /* dt: qcom,mdss-dsi-bl-min-level */
#define LEVEL_MAX                       0x7ff           /* dt: qcom,mdss-dsi-bl-max-level */
#define STEP_MS                         16              /* a fade: one level a frame */
#define FADE_MAX_MS                     3000
#define TOLD                            12              /* requests from iOS told in full */

static uint32_t level, lowpower, off_allowed;
static uint32_t default_level = 1024;
static vm_offset_t dsi;
static volatile bool broken;
static bool usable;

/* what iOS asked for, and what the panel was last given */
static lck_grp_t *bl_group;
static lck_mtx_t *bl_lock;                      /* one command at a time */
static lck_mtx_t *bl_request_lock;            /* what iOS asked for, which serve() sleeps on */
static uint32_t target = ~0u, fade_ms, requests;
static uint32_t shown = ~0u;                    /* the panel's level, ~0u while not known */
static uint64_t commands, slow;
static int request_event;

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
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(dsi + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(dsi + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
spin_us(uint32_t us)
{
	uint64_t until = now_us() + us;

	while (now_us() < until) {
	}
}

static void
show_registers(const char *when)
{
	say("backlight: DSI 0 %s: version %#x ctrl %#x status %#x fifo %#x dma_ctrl %#x dma_len %#x dma_fifo_ctrl %#x "
	    "ack_err %#x trig %#x timeout %#x int %#x tpg %#x tpg_fifo %#x\n", when, rd(DSI_HW_VERSION), rd(DSI_CTRL),
	    rd(DSI_STATUS), rd(DSI_FIFO_STATUS), rd(DSI_COMMAND_MODE_DMA_CTRL), rd(DSI_DMA_CMD_LENGTH), rd(DSI_DMA_FIFO_CTRL),
	    rd(DSI_ACK_ERR_STATUS), rd(DSI_TRIG_CTRL), rd(DSI_TIMEOUT_STATUS), rd(DSI_INT_CTRL),
	    rd(DSI_TEST_PATTERN_GEN_CTRL), rd(DSI_TPG_DMA_FIFO_STATUS));
}

/* DSI 0 as a command can be sent through: on, in command mode, triggered by software, nothing of its own in flight */
static bool
check_controller(void)
{
	uint32_t version = rd(DSI_HW_VERSION), ctrl = rd(DSI_CTRL), trig = rd(DSI_TRIG_CTRL), status = rd(DSI_STATUS);

	if ((version >> 28) != 2) {
		say("backlight: DSI 0's version is %#x, not the 2.x of SM8150's: nothing is written\n", version);
		return false;
	}
	if ((ctrl & 0x5) != 0x5) {
		say("backlight: DSI 0 is %s (ctrl %#x): nothing is written\n", (ctrl & 1) ? "not in command mode" : "off", ctrl);
		return false;
	}
	if ((trig & 0x7) != TRIGGER_SW) {
		say("backlight: DSI 0's command trigger is %u, not software's (%u): nothing is written\n", trig & 0x7, TRIGGER_SW);
		return false;
	}
	if (status & STATUS_DMA_BUSY) {
		say("backlight: DSI 0 is still sending a command of the boot loader's (status %#x): nothing is written\n", status);
		return false;
	}
	return true;
}

/*
 * The panel given one level. A moment after a frame has gone out (MDP busy falling) if the controller is sending
 * frames, at once if it is not; the command DMA's FIFO filled, triggered, waited for, and put back as it was.
 */
static bool
send_level(uint32_t value)
{
	uint32_t words[2], dma_ctrl, tpg, ints, ack, fifo, timeout, t;
	uint64_t start, took;
	bool done = false, was_busy = false;

	if (broken || !usable) {
		return false;
	}
	words[0] = 3u | (uint32_t)DCS_LONG_WRITE << 16 | (HEADER_LONG | HEADER_LAST) << 24;
	words[1] = DCS_SET_DISPLAY_BRIGHTNESS | ((value >> 8) & 0xff) << 8 | (value & 0xff) << 16 | 0xffu << 24;

	lck_mtx_lock(bl_lock);
	ack = rd(DSI_ACK_ERR_STATUS);
	fifo = rd(DSI_FIFO_STATUS);
	timeout = rd(DSI_TIMEOUT_STATUS);

	/* the end of a frame: busy seen, then idle (20 ms at most, a frame is 16.7); with no frames going, idle will do */
	start = now_us();
	while (now_us() - start < 20000) {
		t = rd(DSI_STATUS);
		if (t & STATUS_MDP_BUSY) {
			was_busy = true;
		} else if (was_busy || now_us() - start > 17000) {
			break;
		}
	}
	if (rd(DSI_STATUS) & (STATUS_MDP_BUSY | STATUS_DMA_BUSY)) {
		slow++;
	}

	dma_ctrl = rd(DSI_COMMAND_MODE_DMA_CTRL);
	tpg = rd(DSI_TEST_PATTERN_GEN_CTRL);
	wr(DSI_INT_CTRL, (rd(DSI_INT_CTRL) & INT_ENABLES) | INT_DMA_DONE);
	wr(DSI_TEST_PATTERN_GEN_CTRL, TPG_FIFO_CUSTOM);
	wr(DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, words[0]);
	wr(DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, words[1]);
	wr(DSI_COMMAND_MODE_DMA_CTRL, (dma_ctrl & ~(DMA_CTRL_BROADCAST | DMA_CTRL_LOW_POWER)) | DMA_CTRL_EMBEDDED |
	    (lowpower ? DMA_CTRL_LOW_POWER : 0));
	wr(DSI_DMA_CMD_LENGTH, sizeof(words));
	wr(DSI_CMD_MODE_DMA_SW_TRIGGER, 1);
	start = now_us();
	while ((took = now_us() - start) < 50000) {
		if (rd(DSI_INT_CTRL) & INT_DMA_DONE) {
			done = true;
			break;
		}
	}
	ints = rd(DSI_INT_CTRL);
	wr(DSI_INT_CTRL, (ints & INT_ENABLES) | INT_DMA_DONE);
	wr(DSI_TEST_PATTERN_GEN_CTRL, 0);
	wr(DSI_TPG_DMA_FIFO_RESET, 1);
	spin_us(1);
	wr(DSI_TPG_DMA_FIFO_RESET, 0);
	wr(DSI_TEST_PATTERN_GEN_CTRL, tpg);
	wr(DSI_COMMAND_MODE_DMA_CTRL, dma_ctrl);
	commands++;

	if (!done || rd(DSI_ACK_ERR_STATUS) != ack || (ints & (1u << 24)) || commands <= 2) {
		say("backlight: level %u: %s in %llu us (status %#x, int %#x, ack_err %#x -> %#x, fifo %#x -> %#x, timeout %#x -> %#x)\n",
		    value, done ? "sent" : "NOT SENT", took, rd(DSI_STATUS), ints, ack, rd(DSI_ACK_ERR_STATUS), fifo,
		    rd(DSI_FIFO_STATUS), timeout, rd(DSI_TIMEOUT_STATUS));
	}
	if (!done) {
		broken = true;
		show_registers("after a command that did not finish");
		say("backlight: no more commands are sent in this run\n");
	} else {
		shown = value;
	}
	lck_mtx_unlock(bl_lock);
	return done;
}

static uint32_t
clamp_level(uint32_t value)
{
	if (value == 0 && off_allowed) {
		return 0;
	}
	return value < LEVEL_MIN ? LEVEL_MIN : value > LEVEL_MAX ? LEVEL_MAX : value;
}

/*
 * From the service iOS sets (hakios.backlight.cpp): the brightness it asked for, in the range it was told (0..2047,
 * the panel's levels), and over how long to fade to it (FadePeriod as CoreBrightness gives it, taken as ms).
 */
void hakios_backlight_request(uint32_t value, uint32_t fade, int has_fade);

void
hakios_backlight_request(uint32_t value, uint32_t fade, int has_fade)
{
	uint32_t n;

	lck_mtx_lock(bl_request_lock);
	target = clamp_level(value);
	fade_ms = has_fade ? (fade > FADE_MAX_MS ? FADE_MAX_MS : fade) : 0;
	n = ++requests;
	lck_mtx_unlock(bl_request_lock);
	if (n <= TOLD || n % 100 == 0) {
		say("backlight: request %u from iOS: brightness %u%s, fade %u%s -> level %u\n", n, value,
		    value != clamp_level(value) ? " (out of the panel's range)" : "", fade, has_fade ? " ms" : " (none given)",
		    clamp_level(value));
	}
	wakeup(&request_event);
}

/* The levels iOS asks for, faded to one frame at a time. */
static void
serve(void)
{
	uint32_t from, to, ms, seen = 0;
	uint64_t began;

	for (;;) {
		lck_mtx_lock(bl_request_lock);
		while (requests == seen) {
			(void)msleep(&request_event, bl_request_lock, PRIBIO, "backlight", NULL);
		}
		seen = requests;
		to = target;
		ms = fade_ms;
		lck_mtx_unlock(bl_request_lock);
		if (broken) {
			return;
		}
		from = shown == ~0u ? default_level : shown;
		began = now_us();
		for (;;) {
			uint64_t gone = (now_us() - began) / 1000;
			uint32_t at;
			bool newer;

			if (ms == 0 || gone >= ms) {
				at = to;
			} else {
				at = (uint32_t)((int64_t)from + ((int64_t)to - (int64_t)from) * (int64_t)gone / (int64_t)ms);
			}
			if (at != shown && !send_level(at)) {
				return;
			}
			lck_mtx_lock(bl_request_lock);
			newer = requests != seen;
			lck_mtx_unlock(bl_request_lock);
			if (at == to || newer) {
				break;      /* (a newer request fades on from where this one got to) */
			}
			IOSleep(STEP_MS);
		}
	}
}

static void
backlight_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t test = 0;

	say("backlight: level %u (the EA8076 panel's DCS brightness, through DSI 0): %s\n", level,
	    level == 1 ? "a look, reading only" : "the panel's brightness for iOS");
	dsi = ml_io_map(DSI0_PHYS, 0x1000);
	show_registers("as the boot loader leaves it");
	usable = check_controller();
	if (PE_parse_boot_argn("backlight_test", &test, sizeof(test)) && test != 0 && usable) {
		IOSleep(5000);
		test = clamp_level(test);
		say("backlight: a test: level %u for 3 s, then %u\n", test, default_level);
		if (send_level(test)) {
			show_registers("after the test's command");
			IOSleep(3000);
			(void)send_level(default_level);
		}
	}
	if (level >= 2) {
		if (!usable) {
			say("backlight: no service for iOS: the controller cannot be used\n");
		} else if (HakiosBacklightPublish(0, LEVEL_MAX, shown == ~0u ? default_level : shown) != 0) {
			say("backlight: the service for iOS could not be made\n");
		} else {
			say("backlight: AppleARMBacklight is up for iOS: brightness %u of 0..%u (the panel's levels)%s\n",
			    shown == ~0u ? default_level : shown, LEVEL_MAX, off_allowed ? ", 0 makes the panel dark" : "");
			serve();
		}
	}
	say("backlight: done (%llu commands, %llu sent while a frame was still going out)\n", commands, slow);
	thread_terminate(current_thread());
}

void
hakios_backlight_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("backlight", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 2 ? 2 : level;
	PE_parse_boot_argn("backlight_lp", &lowpower, sizeof(lowpower));
	PE_parse_boot_argn("backlight_off", &off_allowed, sizeof(off_allowed));
	if (PE_parse_boot_argn("backlight_default", &default_level, sizeof(default_level))) {
		default_level = default_level < LEVEL_MIN ? LEVEL_MIN : default_level > LEVEL_MAX ? LEVEL_MAX : default_level;
	}
	bl_group = lck_grp_alloc_init("hakios-backlight", LCK_GRP_ATTR_NULL);
	bl_lock = lck_mtx_alloc_init(bl_group, LCK_ATTR_NULL);
	bl_request_lock = lck_mtx_alloc_init(bl_group, LCK_ATTR_NULL);
	if (kernel_thread_start(backlight_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
