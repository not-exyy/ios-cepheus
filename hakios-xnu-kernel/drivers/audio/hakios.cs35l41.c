/*
 * The bottom speaker of the Xiaomi Mi 9 (cepheus, SM8150): its amplifier, a Cirrus Logic CS35L41 (rev B0,
 * device id 0x35a40: Android's kernel log, "Cirrus Logic CS35L41 (35a40), Revision: B0"), a boosted class-D
 * amplifier with a DSP of its own. Every number below is the phone's own, from its device tree (the dtbo's
 * overlay "CEPHEUS": i2c@890000 cs35l41@40) and Android's kernel log; the registers and the order of things
 * from Linux's sound/soc/codecs/cs35l41.c, cs35l41-lib.c and include/sound/cs35l41.h.
 *
 *   I2C        the serial engine 4 of QUP 0 (i2c@890000; qupv3_se4_i2c: GPIO 51 data and 52 clock, function
 *              qup4, both of the south tile), address 0x40; done by hand, as the other drivers of this port do
 *              it (hakios.speaker.c, hakios.stfts.c): the two pins taken for each transaction and given back as
 *              they were found. A register is 32 bits at a 32-bit address, both most significant byte first.
 *   reset      GPIO 89 (north), active low (cs,cdc-reset-gpio <&tlmm 89 1>)
 *   interrupt  GPIO 10 (north), low when the chip has something to say (not used: polled)
 *   boost      4000 mA peak, 1000 nH, 15 uF (cirrus,boost-peak-milliamp, -ind-nanohenry, -cap-microfarad)
 *   its data   the SoC's quaternary MI2S (GPIO 137.. function qua_mi2s), which the audio DSP drives
 *
 * The phone also has, in its tree, a TI TAS2557 at 0x4c on the same bus for the phones built with that one;
 * on this phone nobody answers there (Android's log: "tas2557 0-004c: I2c fail, -107").
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   speaker_probe=1   a look: the pins, and whether anybody answers at 0x40 (the chip's device id) without the
 *                     reset being touched -- out of the boot loader the chip may be held in reset
 *   speaker_probe=2   ... and the reset released as Linux's cs35l41_probe() releases it (low, 2 ms, high, 2 ms),
 *                     the chip's own start-up waited for (IRQ1_STATUS4 bit 1, "OTP boot done", within 100 ms) and
 *                     its error bit looked at (IRQ1_STATUS3 bit 31), its device and revision ids read; then the
 *                     reset put back as it was found. Nothing is written to the chip.
 *
 * hakios_speaker_prepare() and the rest (sys/hakios.audio.h), for hakios.sound.c's sound=3 and 4: the chip set up
 * as Linux's driver sets one up whose own DSP is not used (the PCM source the ASP, not "DSP" as Android's mixer
 * paths have it: Android runs Cirrus's speaker protection there, cs35l41-dsp1-spk-prot.wmfw, which this does not load
 * -- the samples go through hakios.guard.c instead), with Android's gain:
 *
 *   prepare  the reset released, "OTP boot done" waited for, the ids checked (0x35a40, rev 0xb0); the test key open;
 *            the B0 errata patch and CCM_CORE_CTRL 0 (cs35l41_register_errata_patch); the chip's trims unpacked from
 *            its OTP (cs35l41_otp_unpack, Linux's maps 1 and 2); the test key shut; the interrupt masks; the boost
 *            (cs35l41_boost_config: 1000 nH, 15 uF, 4000 mA peak); the ASP an I2S consumer (the DSP drives the clocks),
 *            48 kHz, slots of 32 bits, words of 24, RX1 from slot 0 (the left: the DSP's one channel); the PLL from the
 *            bit clock at 3.072 MHz (cs35l41_component_set_sysclk, its table: 0x21) and the frame-rate monitor
 *            (cs35l41_dai_set_sysclk: 28, 44); Android's "AMP PCM Gain" 17 (17.5 dB); the digital volume muted; ASPRX1
 *            on, the PCM source ASPRX1 (0x08), class H on. Powered down (GLOBAL_EN 0).
 *   start    only while the bit clock runs: Linux's power-up patch (0x2084), GLOBAL_EN, "power-up done" waited for and
 *            cleared, the amplifier on (PWR_CTRL2 bit 0); still muted
 *   volume   the digital volume, 0.125 dB a step (an eighth of a dB: `below' is in those), stepped 6 dB at a time, never
 *            above 0 dB -- Android's own digital level with its gain of 17
 *   healthy  IRQ1_STATUS1: the amplifier's short circuit, over-temperature, the boost's over-voltage, under-voltage or
 *            short circuit, and both are powered down at once; a temperature warning is passed on (monitor) as 6 dB less
 *   stop     muted stepwise, the amplifier off, GLOBAL_EN 0, "power-down done" waited for, the power-down patch
 *   quiesce  GLOBAL_EN 0 and the reset held, without sleeping: on a panic's way out and at the run's end
 *
 * Every write of a setting is read back (but the test key, the patches and the errata's registers, which Linux writes
 * blind as well); one that does not read back, a byte nobody acknowledges, or a fault, and the chip is powered down and
 * held in reset, and nothing more is done for the rest of the run.
 *
 * Checked: compiled. On the phone (2026-11-04, logs/152738-run.log, speaker_probe=2): out of the boot loader the reset pin
 * is an input with a pull-down and the chip is held in reset (nobody answers); the bus's lines read high (pulled up on the
 * board). Released: "OTP boot done" (IRQ1_STATUS4 0x3) within 2 ms, IRQ1_STATUS3 0x20000000 (no OTP error), device id
 * 0x35a40 and revision 0xb0 -- a CS35L41 B0, as Android says; put back into reset after.
 * sound=3 of hakios.sound.c (logs/154438-run.log): prepared (98 fields of trims from OTP id 1, map 1), started once the
 * bit clock ran ("power-up done", IRQ1_STATUS1 0x1400000), the 1 kHz tone at -20 dBFS 30 dB under Android's level for
 * 3 s with its status looked at 15 times, no fault; stopped. Heard on the phone ("есть звук").
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static vm_offset_t tlmm_north, tlmm_south;
static uint32_t probe_level;

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
	printf("%s", line);
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
wait_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (SM8150's pin controller: four tiles; pinctrl-sm8150.c says which pin is in which) ----------- */

#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_SOUTH_PHYS         0x03d00000UL
#define TLMM_TILE_SIZE          0x100000UL      /* pins up to 255 at 0x1000 each */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 51      /* south */
#define PIN_SCL                 52      /* south */
#define PIN_RESET               89      /* north; low = in reset */
#define PIN_INT                 10      /* north */

static vm_offset_t
gpio_base(unsigned pin)
{
	bool north = pin == PIN_RESET || pin == PIN_INT;

	return (north ? tlmm_north : tlmm_south) + 0x1000UL * pin;
}

#define GPIO_CFG(n)             (gpio_base(n))          /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (gpio_base(n) + 4)      /* bit 0 in, bit 1 out */

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_RESET, "the amplifier's reset" },
	{ PIN_INT, "the amplifier's interrupt" }, { 137, "QUAT MI2S bit clock" }, { 138, "QUAT MI2S word select" },
	{ 139, "QUAT MI2S data 0" }, { 140, "QUAT MI2S data 1" },
};

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("cs35l41: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") :
		    "input", io & 1);
	}
}

/* ---- I2C by hand (as hakios.speaker.c) ---------------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static uint32_t taken_cfg[2], taken_io[2];

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!level(PIN_SDA) || !level(PIN_SCL)) {
		wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
		wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
		wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
		wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

#define CS_ADDRESS              0x40

/* A register of the chip: its 32-bit address, then (after a repeated start) its 32-bit value. False: nobody answered. */
static bool
cs_read(uint32_t reg, uint32_t *value)
{
	bool answered;

	if (!bus_take()) {
		say("cs35l41: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    level(PIN_SDA), PIN_SCL, level(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write(CS_ADDRESS << 1);
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(reg >> shift));
	}
	if (answered) {
		i2c_start();
		answered = i2c_write(CS_ADDRESS << 1 | 1);
	}
	if (answered) {
		uint32_t v = 0;

		for (int i = 0; i < 4; i++) {
			v = v << 8 | i2c_read(i < 3);
		}
		*value = v;
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

/* ---- the chip's registers (include/sound/cs35l41.h) ------------------------------------------------------- */

#define CS35L41_DEVID           0x00000000
#define CS35L41_REVID           0x00000004
#define CS35L41_IRQ1_STATUS3    0x00010018
#define CS35L41_IRQ1_STATUS4    0x0001001c
#define CS35L41_OTP_BOOT_DONE   0x02            /* of IRQ1_STATUS4 */
#define CS35L41_OTP_BOOT_ERR    0x80000000u     /* of IRQ1_STATUS3 */
#define CS35L41_CHIP_ID         0x35a40
#define CS35L41R_CHIP_ID        0x35b40

static bool
show_ids(const char *when)
{
	uint32_t id = 0, rev = 0;

	if (!cs_read(CS35L41_DEVID, &id)) {
		say("cs35l41: %s: nobody answers at 0x%02x\n", when, CS_ADDRESS);
		return false;
	}
	(void)cs_read(CS35L41_REVID, &rev);
	say("cs35l41: %s: device id %#x (%s), revision %#x\n", when, id,
	    id == CS35L41_CHIP_ID ? "a CS35L41" : id == CS35L41R_CHIP_ID ? "a CS35L41R" : "NOT a CS35L41", rev);
	return id == CS35L41_CHIP_ID || id == CS35L41R_CHIP_ID;
}


/* ---- the amplifier for the sound (sys/hakios.audio.h) --------------------------------------------------------- */

static bool leaving, broken, prepared, started;

static void
pause_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		wait_ms(ms);
	}
}

/* A register written: false if nobody answers. */
static bool
cs_write(uint32_t reg, uint32_t value)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(CS_ADDRESS << 1);
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(reg >> shift));
	}
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(value >> shift));
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static void
give_up(const char *why)
{
	if (broken) {
		return;
	}
	broken = true;
	say("cs35l41: GIVEN UP: %s; the amplifier is powered down and held in reset, and nothing more is done\n", why);
	(void)cs_write(0x00002014 /* PWR_CTRL1 */, 0);
	wr(GPIO_IO(PIN_RESET), 0);
	started = false;
}

/* written blind (the test key, the patches, the errata), as Linux writes them */
static bool
put_blind(uint32_t reg, uint32_t value)
{
	if (broken) {
		return false;
	}
	if (!cs_write(reg, value)) {
		say("cs35l41: writing %#x to %#x: not acknowledged\n", value, reg);
		give_up("a write was not acknowledged");
		return false;
	}
	return true;
}

/* the bits of `mask' to `value', the others as they are; read back unless `blind' */
static bool
modify(uint32_t reg, uint32_t mask, uint32_t value, bool blind)
{
	uint32_t now = 0, back = 0, want;

	if (broken) {
		return false;
	}
	if (!cs_read(reg, &now)) {
		say("cs35l41: reading %#x: nobody answers\n", reg);
		give_up("a read was not answered");
		return false;
	}
	want = (now & ~mask) | (value & mask);
	if (want == now) {
		return true;
	}
	if (!put_blind(reg, want)) {
		return false;
	}
	if (blind) {
		return true;
	}
	if (cs_read(reg, &back) && ((back ^ want) & mask) == 0) {
		return true;
	}
	/*
	 * Once, in sound=4 (logs/154641), AMP_DIG_VOL_CTRL read back 0 after the second start -- every bit, the high-pass's
	 * too, which nothing had cleared: a read gone wrong, it seems, not the register. So: read again (three times, a
	 * millisecond apart), then write it once more; only then is the chip given up on.
	 */
	{
		uint32_t first = back;

		for (unsigned again = 0; again < 3; again++) {
			pause_ms(1);
			if (cs_read(reg, &back) && ((back ^ want) & mask) == 0) {
				say("cs35l41: register %#x: %#x written, %#x read back, then %#x read again (%u): taken\n", reg, want, first, back,
				    again + 1);
				return true;
			}
		}
		if (put_blind(reg, want) && cs_read(reg, &back) && ((back ^ want) & mask) == 0) {
			say("cs35l41: register %#x: %#x written, %#x read back; written again, %#x: taken\n", reg, want, first, back);
			return true;
		}
		{
			uint32_t id = 0, status = 0;

			(void)cs_read(CS35L41_DEVID, &id);
			(void)cs_read(0x00010010 /* IRQ1_STATUS1 */, &status);
			say("cs35l41: register %#x: %#x written, %#x read back, %#x after writing again (the device id reads %#x, IRQ1_STATUS1 "
			    "%#x)\n", reg, want, first, back, id, status);
		}
	}
	give_up("a write did not read back");
	return false;
}

static bool
update(uint32_t reg, uint32_t mask, uint32_t value)
{
	return modify(reg, mask, value, false);
}

static bool
put(uint32_t reg, uint32_t value)
{
	return update(reg, 0xffffffffu, value);
}

/* the chip's registers (include/sound/cs35l41.h) */
#define CS35L41_OTPID                   0x00000010
#define CS35L41_TEST_KEY_CTL            0x00000040
#define CS35L41_OTP_MEM0                0x00000400
#define CS35L41_PWR_CTRL1               0x00002014      /* [0] GLOBAL_EN */
#define CS35L41_PWR_CTRL2               0x00002018      /* [0] AMP_EN, [5:4] BST_EN */
#define CS35L41_PWR_CTRL3               0x0000201c      /* [4] CLASS H */
#define CS35L41_PLL_CLK_CTRL            0x00002c04
#define CS35L41_GLOBAL_CLK_CTRL         0x00002c0c
#define CS35L41_TST_FS_MON0             0x00002d10
#define CS35L41_BSTCVRT_PEAK_CUR        0x00003808
#define CS35L41_BSTCVRT_COEFF           0x00003810
#define CS35L41_BSTCVRT_SLOPE_LBST      0x00003814
#define CS35L41_BSTCVRT_DCM_CTRL        0x0000381c
#define CS35L41_VIMON_SPKMON_RESYNC     0x00004100
#define CS35L41_VPVBST_FS_SEL           0x00004400
#define CS35L41_SP_ENABLES              0x00004800      /* [16] ASPRX1 */
#define CS35L41_SP_FORMAT               0x00004808
#define CS35L41_SP_FRAME_RX_SLOT        0x00004820
#define CS35L41_SP_RX_WL                0x00004840
#define CS35L41_DAC_PCM1_SRC            0x00004c00
#define CS35L41_ASP_TX3_SRC             0x00004c28
#define CS35L41_ASP_TX4_SRC             0x00004c2c
#define CS35L41_AMP_DIG_VOL_CTRL        0x00006000      /* [13:3] the volume, 0.125 dB, signed; [15] the high-pass on */
#define CS35L41_AMP_GAIN_CTRL           0x00006c04      /* [9:5] AMP_GAIN_PCM */
#define CS35L41_IRQ1_STATUS1            0x00010010
#define CS35L41_IRQ1_MASK1              0x00010110
#define CS35L41_DSP1_CCM_CORE_CTRL      0x02bc1000
#define CS35L41_DSP1_XM_ACCEL_PL0_PRI   0x02bc2020
#define CS35L41_DSP1_YM_ACCEL_PL0_PRI   0x02bc20e0

#define INT1_MASK_DEFAULT               0x7ffcfe3fu
#define PUP_DONE                        0x01000000u
#define PDN_DONE                        0x00800000u
#define STATUS1_FAULTS                  (0x80000000u /* amp short */ | 0x00020000u /* temp */ | 0x40u /* bst ovp */ | \
	                                 0x80u /* bst dcm uvp */ | 0x100u /* bst short */)
#define STATUS1_TEMP_WARN               0x8000u
#define VOL_SHIFT                       3
#define VOL_MASK                        (0x7ffu << VOL_SHIFT)
#define VOL_MUTE                        0x4cfu          /* CS35L41_AMP_PCM_VOL_MUTE */
#define VOL_LOWEST                      816             /* -102 dB, in eighths */
#define ANDROID_GAIN                    17              /* the mixer paths' "AMP PCM Gain": 17.5 dB */

/* Linux's OTP maps (cs35l41-lib.c otp_map_1, otp_map_2: register, shift, size; register 0: bits skipped) */
struct otp_element {
	uint32_t reg;
	uint8_t shift, size;
};
static const struct otp_element otp_map_1[] = {
	{ 0x2030, 0, 4 }, { 0x2030, 7, 1 }, { 0x208c, 24, 6 }, { 0x2090, 14, 4 }, { 0x2090, 10, 4 },
	{ 0x300c, 11, 4 }, { 0x394c, 23, 2 }, { 0x3950, 0, 7 }, { 0x3950, 8, 7 }, { 0x3950, 16, 8 },
	{ 0x3950, 24, 8 }, { 0x3954, 0, 7 }, { 0x3954, 8, 7 }, { 0x3954, 16, 8 }, { 0x3954, 24, 8 },
	{ 0x3958, 0, 7 }, { 0x3958, 8, 7 }, { 0x3958, 16, 8 }, { 0x3958, 24, 8 }, { 0x395c, 0, 7 }, { 0x395c, 8, 7 },
	{ 0x395c, 16, 8 }, { 0x395c, 24, 8 }, { 0x416c, 0, 8 }, { 0x4160, 0, 7 }, { 0x416c, 8, 8 },
	{ 0x4160, 16, 10 }, { 0x416c, 16, 12 }, { 0x416c, 28, 1 }, { 0x4170, 0, 6 }, { 0x4170, 6, 1 },
	{ 0x4170, 8, 6 }, { 0x4170, 14, 1 }, { 0x4170, 16, 9 }, { 0x4360, 0, 5 }, { 0x4360, 6, 9 }, { 0x4448, 0, 8 },
	{ 0x4448, 8, 8 }, { 0x4448, 16, 8 }, { 0x4448, 24, 8 }, { 0x444c, 0, 3 }, { 0x6e30, 0, 5 }, { 0x6e30, 8, 5 },
	{ 0x6e30, 16, 5 }, { 0x6e30, 24, 5 }, { 0x6e34, 0, 5 }, { 0x6e34, 8, 5 }, { 0x6e34, 16, 5 },
	{ 0x6e34, 24, 5 }, { 0x6e38, 0, 5 }, { 0x6e38, 8, 5 }, { 0x6e38, 16, 5 }, { 0x6e38, 24, 5 },
	{ 0x6e3c, 0, 5 }, { 0x6e3c, 8, 5 }, { 0x6e3c, 16, 5 }, { 0x6e3c, 24, 5 }, { 0x6e40, 0, 5 }, { 0x6e40, 8, 5 },
	{ 0x6e40, 16, 5 }, { 0x6e40, 24, 5 }, { 0x6e44, 0, 5 }, { 0x6e48, 0, 10 }, { 0x6e48, 10, 10 },
	{ 0x6e48, 20, 10 }, { 0x6e4c, 0, 10 }, { 0x6e4c, 10, 10 }, { 0x6e4c, 20, 10 }, { 0x6e50, 0, 10 },
	{ 0x6e50, 10, 10 }, { 0x6e50, 20, 10 }, { 0x6e54, 0, 10 }, { 0x6e54, 10, 10 }, { 0x6e54, 20, 10 },
	{ 0x6e58, 0, 10 }, { 0x6e58, 10, 10 }, { 0x6e58, 20, 10 }, { 0x6e5c, 0, 10 }, { 0x6e5c, 10, 10 },
	{ 0x6e5c, 20, 10 }, { 0x6e60, 0, 10 }, { 0x6e60, 10, 10 }, { 0x6e60, 20, 10 }, { 0x6e64, 0, 10 },
	{ 0x7418, 7, 5 }, { 0x741c, 0, 5 }, { 0x741c, 11, 4 }, { 0x741c, 19, 4 }, { 0x7434, 17, 1 },
	{ 0x7434, 18, 7 }, { 0x7068, 0, 9 }, { 0x410c, 7, 1 }, { 0x400c, 0, 7 }, { 0x0000, 0, 1 },
	{ 0x00017040, 0, 8 }, { 0x00017040, 8, 8 }, { 0x00017040, 16, 8 }, { 0x00017040, 24, 8 },
	{ 0x00017044, 0, 24 },
};
static const struct otp_element otp_map_2[] = {
	{ 0x2030, 0, 4 }, { 0x2030, 7, 1 }, { 0x208c, 24, 6 }, { 0x2090, 14, 4 }, { 0x2090, 10, 4 },
	{ 0x300c, 11, 4 }, { 0x394c, 23, 2 }, { 0x3950, 0, 7 }, { 0x3950, 8, 7 }, { 0x3950, 16, 8 },
	{ 0x3950, 24, 8 }, { 0x3954, 0, 7 }, { 0x3954, 8, 7 }, { 0x3954, 16, 8 }, { 0x3954, 24, 8 },
	{ 0x3958, 0, 7 }, { 0x3958, 8, 7 }, { 0x3958, 16, 8 }, { 0x3958, 24, 8 }, { 0x395c, 0, 7 }, { 0x395c, 8, 7 },
	{ 0x395c, 16, 8 }, { 0x395c, 24, 8 }, { 0x416c, 0, 8 }, { 0x4160, 0, 7 }, { 0x416c, 8, 8 },
	{ 0x4160, 16, 10 }, { 0x416c, 16, 12 }, { 0x416c, 28, 1 }, { 0x4170, 0, 6 }, { 0x4170, 6, 1 },
	{ 0x4170, 8, 6 }, { 0x4170, 14, 1 }, { 0x4170, 16, 9 }, { 0x4360, 0, 5 }, { 0x4360, 6, 9 }, { 0x4448, 0, 8 },
	{ 0x4448, 8, 8 }, { 0x4448, 16, 8 }, { 0x4448, 24, 8 }, { 0x444c, 0, 3 }, { 0x6e30, 0, 5 }, { 0x6e30, 8, 5 },
	{ 0x6e30, 16, 5 }, { 0x6e30, 24, 5 }, { 0x6e34, 0, 5 }, { 0x6e34, 8, 5 }, { 0x6e34, 16, 5 },
	{ 0x6e34, 24, 5 }, { 0x6e38, 0, 5 }, { 0x6e38, 8, 5 }, { 0x6e38, 16, 5 }, { 0x6e38, 24, 5 },
	{ 0x6e3c, 0, 5 }, { 0x6e3c, 8, 5 }, { 0x6e3c, 16, 5 }, { 0x6e3c, 24, 5 }, { 0x6e40, 0, 5 }, { 0x6e40, 8, 5 },
	{ 0x6e40, 16, 5 }, { 0x6e40, 24, 5 }, { 0x6e44, 0, 5 }, { 0x6e48, 0, 10 }, { 0x6e48, 10, 10 },
	{ 0x6e48, 20, 10 }, { 0x6e4c, 0, 10 }, { 0x6e4c, 10, 10 }, { 0x6e4c, 20, 10 }, { 0x6e50, 0, 10 },
	{ 0x6e50, 10, 10 }, { 0x6e50, 20, 10 }, { 0x6e54, 0, 10 }, { 0x6e54, 10, 10 }, { 0x6e54, 20, 10 },
	{ 0x6e58, 0, 10 }, { 0x6e58, 10, 10 }, { 0x6e58, 20, 10 }, { 0x6e5c, 0, 10 }, { 0x6e5c, 10, 10 },
	{ 0x6e5c, 20, 10 }, { 0x6e60, 0, 10 }, { 0x6e60, 10, 10 }, { 0x6e60, 20, 10 }, { 0x6e64, 0, 10 },
	{ 0x7418, 7, 5 }, { 0x741c, 0, 5 }, { 0x741c, 11, 4 }, { 0x741c, 19, 4 }, { 0x7434, 17, 1 },
	{ 0x7434, 18, 7 }, { 0x7068, 0, 9 }, { 0x410c, 7, 1 }, { 0x400c, 0, 7 }, { 0x4000, 11, 1 },
	{ 0x00017040, 0, 8 }, { 0x00017040, 8, 8 }, { 0x00017040, 16, 8 }, { 0x00017040, 24, 8 },
	{ 0x00017044, 0, 24 },
};

static bool
otp_unpack(void)
{
	const struct otp_element *map;
	uint32_t id = 0, mem[32], bit_offset = 16, word = 2, written = 0;
	unsigned count;

	if (!cs_read(CS35L41_OTPID, &id)) {
		give_up("the OTP's id cannot be read");
		return false;
	}
	switch (id) {
	case 0x01: case 0x08:
		map = otp_map_1;
		count = sizeof(otp_map_1) / sizeof(otp_map_1[0]);
		break;
	case 0x02: case 0x03: case 0x06:
		map = otp_map_2;
		count = sizeof(otp_map_2) / sizeof(otp_map_2[0]);
		break;
	default:
		say("cs35l41: OTP id %#x: no map for it\n", id);
		give_up("an OTP map this does not know");
		return false;
	}
	for (unsigned i = 0; i < 32; i++) {
		if (!cs_read(CS35L41_OTP_MEM0 + 4 * i, &mem[i])) {
			give_up("the OTP cannot be read");
			return false;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		uint32_t size = map[i].size, value = 0;

		if (bit_offset + size >= 33) {          /* (Linux: bit_offset + size - 1 >= 32) */
			value = (mem[word] >> bit_offset);
			if (word + 1 >= 32) {
				give_up("the OTP map runs past the OTP");
				return false;
			}
			word++;
			value |= (mem[word] & (0xffffffffu >> (64 - bit_offset - size))) << (32 - bit_offset);
			bit_offset += size - 32;
		} else if (size != 0) {
			value = (mem[word] >> bit_offset) & (0xffffffffu >> (32 - size));
			bit_offset += size;
		}
		if (bit_offset == 32) {
			bit_offset = 0;
			word++;
		}
		if (map[i].reg != 0 && size != 0) {
			uint32_t mask = (0xffffffffu >> (32 - size)) << map[i].shift;

			/* (as Linux: regmap_update_bits, nothing read back) */
			if (!modify(map[i].reg, mask, value << map[i].shift, true)) {
				return false;
			}
			written++;
		}
	}
	say("cs35l41: the trims unpacked from the OTP (id %#x, map %u): %u fields\n", id, map == otp_map_1 ? 1 : 2, written);
	return true;
}

static void
need_pins(void)
{
	if (tlmm_north == 0) {
		tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
		tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	}
}

static uint32_t volume_now = VOL_LOWEST + 1;    /* eighths of a dB under 0 dB; VOL_LOWEST + 1: muted */

static bool
volume_put(uint32_t below)
{
	uint32_t field = below > VOL_LOWEST ? VOL_MUTE : (uint32_t)(-(int32_t)below) & 0x7ffu;

	if (!update(CS35L41_AMP_DIG_VOL_CTRL, VOL_MASK, field << VOL_SHIFT)) {
		return false;
	}
	volume_now = below > VOL_LOWEST ? VOL_LOWEST + 1 : below;
	return true;
}

bool
hakios_speaker_prepare(__unused const uint8_t *acf, __unused uint64_t size)
{
	static const uint32_t errata_b0[][2] = {
		{ CS35L41_VIMON_SPKMON_RESYNC, 0 }, { 0x00004310, 0 }, { CS35L41_VPVBST_FS_SEL, 0 }, { CS35L41_BSTCVRT_DCM_CTRL, 0x51 },
		{ CS35L41_DSP1_YM_ACCEL_PL0_PRI, 0 }, { CS35L41_DSP1_XM_ACCEL_PL0_PRI, 0 }, { CS35L41_PWR_CTRL2, 0 },
		{ CS35L41_AMP_GAIN_CTRL, 0 }, { CS35L41_ASP_TX3_SRC, 0 }, { CS35L41_ASP_TX4_SRC, 0 },
	};
	uint32_t status = 0, errors = 0, id = 0, rev = 0;

	if (broken) {
		return false;
	}
	if (prepared) {
		return true;
	}
	need_pins();
	say("cs35l41: prepared as Linux prepares one whose DSP is not used: the reset released\n");
	wr(GPIO_IO(PIN_RESET), 0);
	wr(GPIO_CFG(PIN_RESET), (rd(GPIO_CFG(PIN_RESET)) & ~(3u | 15u << 2)) | CFG_OUTPUT);
	wait_ms(2);
	wr(GPIO_IO(PIN_RESET), 2);
	wait_ms(2);
	for (unsigned ms = 0; ms < 100 && !(cs_read(CS35L41_IRQ1_STATUS4, &status) && (status & CS35L41_OTP_BOOT_DONE)); ms++) {
		wait_ms(1);
	}
	(void)cs_read(CS35L41_IRQ1_STATUS3, &errors);
	if (!(status & CS35L41_OTP_BOOT_DONE) || (errors & CS35L41_OTP_BOOT_ERR) || !cs_read(CS35L41_DEVID, &id) ||
	    !cs_read(CS35L41_REVID, &rev) || id != CS35L41_CHIP_ID || rev != 0xb0) {
		say("cs35l41: its start-up: IRQ1_STATUS4 %#x, IRQ1_STATUS3 %#x, device id %#x, revision %#x\n", status, errors, id, rev);
		give_up("not the chip this is for, or it did not start");
		return false;
	}
	say("cs35l41: a CS35L41 B0, started; the test key open, the B0 errata, the trims from the OTP\n");
	if (!put_blind(CS35L41_TEST_KEY_CTL, 0x55) || !put_blind(CS35L41_TEST_KEY_CTL, 0xaa)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(errata_b0) / sizeof(errata_b0[0]); i++) {
		if (!put_blind(errata_b0[i][0], errata_b0[i][1])) {
			return false;
		}
	}
	if (!put_blind(CS35L41_DSP1_CCM_CORE_CTRL, 0) || !otp_unpack() ||
	    !put_blind(CS35L41_TEST_KEY_CTL, 0xcc) || !put_blind(CS35L41_TEST_KEY_CTL, 0x33)) {
		return false;
	}
	say("cs35l41: the boost (1000 nH, 15 uF, 4000 mA), the ASP (I2S, 48 kHz, 32-bit slots of 24-bit words, RX1 slot 0), the "
	    "PLL from the 3.072 MHz bit clock, gain 17, muted, ASPRX1 to the PCM, class H\n");
	if (!put(CS35L41_IRQ1_MASK1, INT1_MASK_DEFAULT) ||
	    /* cs35l41_boost_config(1000, 15, 4000): k1 0x24, k2 0x24, slope 0x75, lbst 0, ipk (4000 - 1600) / 50 + 0x10 */
	    !update(CS35L41_BSTCVRT_COEFF, 0xffff, 0x24 | 0x24 << 8) ||
	    !update(CS35L41_BSTCVRT_SLOPE_LBST, 0xff03, 0x75 << 8 | 0) ||
	    !update(CS35L41_BSTCVRT_PEAK_CUR, 0x7f, (4000 - 1600) / 50 + 0x10) ||
	    !update(CS35L41_PWR_CTRL2, 0x30, 2 << 4) ||
	    /* cs35l41_set_dai_fmt(CBC_CFC | I2S | NB_NF): the format 2, nothing ours */
	    !update(CS35L41_SP_FORMAT, 0x10 | 0x01 | 0x0700 | 0x04 | 0x40, 2 << 8) ||
	    /* cs35l41_pcm_hw_params: 48 kHz (0x03); the RX slot 32 bits wide, the word 24 */
	    !update(CS35L41_GLOBAL_CLK_CTRL, 0x1f, 0x03) ||
	    !update(CS35L41_SP_FORMAT, 0xff000000u, 32u << 24) ||
	    !update(CS35L41_SP_RX_WL, 0x3f, 24) ||
	    !update(CS35L41_SP_FRAME_RX_SLOT, 0x3f, 0) ||
	    /* cs35l41_component_set_sysclk(SCLK, 3072000): open loop, the reference 0x21, off, from SCLK, closed loop, on */
	    !update(CS35L41_PLL_CLK_CTRL, 0x0800, 0x0800) ||
	    !update(CS35L41_PLL_CLK_CTRL, 0x07e0, 0x21 << 5) ||
	    !update(CS35L41_PLL_CLK_CTRL, 0x0010, 0) ||
	    !update(CS35L41_PLL_CLK_CTRL, 0x0007, 0) ||
	    !update(CS35L41_PLL_CLK_CTRL, 0x0800, 0) ||
	    !update(CS35L41_PLL_CLK_CTRL, 0x0010, 0x0010) ||
	    /* cs35l41_dai_set_sysclk(3072000) */
	    !put(CS35L41_TST_FS_MON0, 28 | (44u << 12 & 0x00fff800u)) ||
	    !update(CS35L41_AMP_GAIN_CTRL, 0x3e0, ANDROID_GAIN << 5) ||
	    !volume_put(VOL_LOWEST + 1) ||
	    !update(CS35L41_DAC_PCM1_SRC, 0xff, 0x08) ||
	    !update(CS35L41_SP_ENABLES, 1u << 16, 1u << 16) ||
	    !update(CS35L41_PWR_CTRL3, 1u << 4, 1u << 4)) {
		return false;
	}
	prepared = true;
	say("cs35l41: PREPARED: powered down, muted\n");
	return true;
}

bool
hakios_speaker_start(void)
{
	uint32_t status = 0;

	if (broken || !prepared) {
		return false;
	}
	if (started) {
		return true;
	}
	/* cs35l41_main_amp_event(PRE_PMU): the power-up patch, then cs35l41_global_enable() for the internal boost */
	if (!put_blind(CS35L41_TEST_KEY_CTL, 0x55) || !put_blind(CS35L41_TEST_KEY_CTL, 0xaa) ||
	    !put_blind(0x00002084, 0x002f1aa0) || !put_blind(CS35L41_TEST_KEY_CTL, 0xcc) || !put_blind(CS35L41_TEST_KEY_CTL, 0x33) ||
	    !update(CS35L41_PWR_CTRL1, 1, 1)) {
		return false;
	}
	for (unsigned ms = 0; ms < 100 && !(cs_read(CS35L41_IRQ1_STATUS1, &status) && (status & PUP_DONE)); ms++) {
		pause_ms(1);
	}
	if (!(status & PUP_DONE)) {
		say("cs35l41: no \"power-up done\" within 100 ms (IRQ1_STATUS1 %#x): is the bit clock running?\n", status);
		give_up("it did not power up");
		return false;
	}
	if (!put_blind(CS35L41_IRQ1_STATUS1, PUP_DONE) || !update(CS35L41_PWR_CTRL2, 1, 1)) {
		return false;
	}
	started = true;
	say("cs35l41: STARTED (IRQ1_STATUS1 %#x): powered up, the amplifier on, muted\n", status);
	return true;
}

bool
hakios_speaker_volume(uint32_t below, uint32_t step_ms)
{
	uint32_t target = below > VOL_LOWEST ? VOL_LOWEST + 1 : below;

	if (broken || !started) {
		return false;
	}
	while (volume_now != target) {
		uint32_t next;

		if (volume_now > target) {
			next = volume_now - target > 48 ? volume_now - 48 : target;
			if (volume_now == VOL_LOWEST + 1) {
				next = target + 48 * 4 < VOL_LOWEST ? target + 48 * 4 : target; /* from mute: 24 dB above the end at most */
				next = next > VOL_LOWEST ? VOL_LOWEST : next;
			}
		} else {
			next = target - volume_now > 48 ? volume_now + 48 : target;
		}
		if (!volume_put(next)) {
			return false;
		}
		if (volume_now != target && step_ms != 0) {
			pause_ms(step_ms);
		}
	}
	return true;
}

static uint32_t warnings;

bool
hakios_speaker_healthy(void)
{
	uint32_t status = 0;

	if (broken) {
		return false;
	}
	if (!cs_read(CS35L41_IRQ1_STATUS1, &status)) {
		give_up("its status cannot be read");
		return false;
	}
	if (status & STATUS1_FAULTS) {
		say("cs35l41: IRQ1_STATUS1 %#x: A FAULT (%s%s%s%s%s)\n", status, status & 0x80000000u ? "the amplifier's short circuit " : "",
		    status & 0x00020000u ? "over-temperature " : "", status & 0x40u ? "the boost's over-voltage " : "",
		    status & 0x80u ? "the boost's under-voltage " : "", status & 0x100u ? "the boost's short circuit" : "");
		give_up("a fault");
		return false;
	}
	warnings = status & STATUS1_TEMP_WARN;
	return true;
}

uint32_t
hakios_speaker_monitor(bool tell)
{
	if (warnings && tell) {
		say("cs35l41: a temperature warning: 6 dB less\n");
	}
	return warnings ? 48 : 0;
}

void
hakios_speaker_stop(void)
{
	uint32_t status = 0;

	if (broken || !started) {
		return;
	}
	(void)hakios_speaker_volume(VOL_LOWEST + 1, 1);
	/* the amplifier off, then cs35l41_global_enable(0) and the power-down patch (POST_PMD) */
	if (!update(CS35L41_PWR_CTRL2, 1, 0) || !update(CS35L41_PWR_CTRL1, 1, 0)) {
		return;
	}
	for (unsigned ms = 0; ms < 100 && !(cs_read(CS35L41_IRQ1_STATUS1, &status) && (status & PDN_DONE)); ms++) {
		pause_ms(1);
	}
	(void)put_blind(CS35L41_IRQ1_STATUS1, PDN_DONE);
	(void)put_blind(CS35L41_TEST_KEY_CTL, 0x55);
	(void)put_blind(CS35L41_TEST_KEY_CTL, 0xaa);
	(void)put_blind(0x00002084, 0x002f1aa3);
	(void)put_blind(CS35L41_TEST_KEY_CTL, 0xcc);
	(void)put_blind(CS35L41_TEST_KEY_CTL, 0x33);
	started = false;
	say("cs35l41: stopped: muted, the amplifier off, powered down%s\n", (status & PDN_DONE) ? "" : " (no \"power-down done\")");
}

void
hakios_speaker_quiesce(void)
{
	if (!prepared || tlmm_north == 0) {
		return;
	}
	leaving = true;
	(void)cs_write(CS35L41_PWR_CTRL1, 0);
	wr(GPIO_IO(PIN_RESET), 0);
	prepared = started = false;
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t reset_cfg, reset_io;
	bool was_released;

	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	say("cs35l41: level %u: %s\n", probe_level, probe_level == 1 ? "a look, reading only" :
	    "the reset released, the chip's start-up waited for, its ids read; nothing written to it");
	show_pins();
	reset_cfg = rd(GPIO_CFG(PIN_RESET));
	reset_io = rd(GPIO_IO(PIN_RESET));
	was_released = (reset_cfg & CFG_OUTPUT) && (reset_io & 2);
	say("cs35l41: the reset is %s\n", was_released ? "released (gpio89 drives high)" : "not released: the chip is held in reset");
	(void)show_ids("as found");
	if (probe_level >= 2) {
		uint32_t status = 0, errors = 0;
		uint64_t began;

		say("cs35l41: the reset is released as Linux does it: gpio89 low for 2 ms, then high\n");
		wr(GPIO_IO(PIN_RESET), 0);
		wr(GPIO_CFG(PIN_RESET), (reset_cfg & ~(3u | 15u << 2)) | CFG_OUTPUT);   /* a GPIO, no pull, output, its drive kept */
		wait_ms(2);
		wr(GPIO_IO(PIN_RESET), 2);
		wait_ms(2);
		began = mach_absolute_time();
		for (unsigned ms = 0; ms < 100; ms++) {
			if (cs_read(CS35L41_IRQ1_STATUS4, &status) && (status & CS35L41_OTP_BOOT_DONE)) {
				break;
			}
			wait_ms(1);
		}
		(void)cs_read(CS35L41_IRQ1_STATUS3, &errors);
		{
			uint64_t ns;

			absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
			say("cs35l41: its start-up: IRQ1_STATUS4 %#x (%s) after %llu ms, IRQ1_STATUS3 %#x%s\n", status,
			    (status & CS35L41_OTP_BOOT_DONE) ? "OTP boot done" : "OTP BOOT NOT DONE", ns / NSEC_PER_MSEC, errors,
			    (errors & CS35L41_OTP_BOOT_ERR) ? " (OTP BOOT ERROR)" : "");
		}
		(void)show_ids("out of reset");
		/* back as it was found */
		wr(GPIO_IO(PIN_RESET), reset_io & 2);
		wr(GPIO_CFG(PIN_RESET), reset_cfg);
		say("cs35l41: the reset is put back as it was found (%s)\n", was_released ? "released" : "held");
	}
	say("cs35l41: done\n");
	thread_terminate(current_thread());
}

void
hakios_speaker_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("speaker_probe", &probe_level, sizeof(probe_level)) || probe_level == 0) {
		return;
	}
	probe_level = probe_level > 2 ? 2 : probe_level;
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
