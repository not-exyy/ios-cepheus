/*
 * The touch screen of the Xiaomi Mi 9 (cepheus): an STMicroelectronics FingerTip (FTM5, "st,fts") on I2C at
 * address 0x49, as a source of touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg
 * touchscreen=1: every address below belongs to this phone.
 *
 * As in hakios.goodix.c there is no clock, pin, regulator or I2C framework in this kernel, so the driver does
 * what it needs itself, and as little of it as it can:
 *   bus      I2C on GPIO55 (SDA) and GPIO56 (SCL), the pins of serial engine 17, driven by hand as an open-drain
 *            bus (low: the pin drives 0; high: the pin lets go and the pull-ups take the line up). The serial
 *            engine, its clocks and its firmware are never touched; a report is a few dozen bytes, so 100 kHz
 *            is plenty. If nothing answers with 55 as SDA, the other way round is tried before giving up
 *   power    the chip's I/O supply is the display's 1.8 V switch behind GPIO84 ("disp_vddio_vreg", on since the
 *            boot loader lit the panel); the analog supply is LDO1 of the PM8150L ("ldoc1"), an on/off switch of
 *            RPMh ("xob": no voltage is chosen, there is none to choose). Both are only turned on when the chip
 *            does not answer as it is, in the order of Xiaomi's driver: I/O, 1 ms, analog, 5 ms
 *   pins     reset GPIO12 (low = in reset), interrupt GPIO122 (low = events wait in the chip's FIFO)
 *   chip     a reset, the "controller ready" event, the chip's id (0x36 0x39) read back, then active scanning
 *            (command A0 00 01); then the FIFO (command 0x86) is read whenever the interrupt line is low
 * No interrupt is used: a thread looks at the interrupt line 125 times a second (touch_poll_us=N: every N
 * microseconds instead), and reads the FIFO every 80 ms anyway in case a low line is missed.
 * touch_flip=1/2/3 mirrors x, y or both, as in hakios.goodix.c.
 *
 * Event layout, commands and registers: Xiaomi's driver for this phone (LineageOS android_kernel_xiaomi_sm8150,
 * drivers/input/touchscreen/xiaomi/fts_521: fts.c, fts_lib/ftsCore.c, ftsHardware.h, ftsSoftware.h); the pins,
 * supplies and sizes from the phone's own device tree (devices/cepheus/prebuilt/device-tree.dtb, i2c@0xc80000/fts@49);
 * the pin tiles from pinctrl-sm8150.c; RPMh and its command database as in hakios.goodix.c.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* RPMh's sequencer (TCS 0 of the RSC): the other drivers that write the same slots take this lock too */
extern lck_mtx_t hakios_rsc_lock;

static vm_offset_t tlmm_north, tlmm_south, rsc, cmd_db;

/*
 * Every line to the loader's log as well: the kernel's own buffer reaches the host only at the run's end, and by then
 * the bring-up's lines are gone from it (2026-11-04: the touch screen silent, and nothing left to say why).
 */
extern void loader_log_putc(char c);

static int stfts_say(const char *format, ...) __printflike(1, 2);

static int
stfts_say(const char *format, ...)
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
	return printf("%s", line);
}
#define printf stfts_say

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
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* A busy wait, for the bus's half periods: a few microseconds, which a sleep would stretch to far more. */
static uint64_t ticks_per_us;

static void
spin_us(uint32_t us)
{
	uint64_t end = mach_absolute_time() + us * ticks_per_us;

	while (mach_absolute_time() < end) {
	}
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

/* SM8150's pin controller is four tiles; a pin's registers are 0x1000 * its number into its own tile */
#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_SOUTH_PHYS         0x03d00000UL
#define TLMM_TILE_SIZE          0x80000UL       /* pins up to 127 */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

#define PIN_RESET               12      /* north; low = in reset */
#define PIN_SDA                 55      /* south; serial engine 17's I2C, the first of its two pins */
#define PIN_SCL                 56      /* south */
#define PIN_VDDIO               84      /* north; high = the 1.8 V I/O supply on (named "disp_vddio_vreg", but only the touch
                                         * screen's: the panel stays lit with it off) */
#define PIN_INT                 122     /* south; low = events wait */

static vm_offset_t
gpio_base(unsigned pin)
{
	bool north = pin == PIN_RESET || pin == PIN_VDDIO;      /* the only north pins here (pinctrl-sm8150.c) */

	return (north ? tlmm_north : tlmm_south) + 0x1000UL * pin;
}

#define GPIO_CFG(n)             (gpio_base(n))          /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (gpio_base(n) + 4)      /* bit 0 in, bit 1 out */
#define CFG_OUTPUT              (1u << 9)

static void
gpio_configure(unsigned pin, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | 0 << 2 | (ma / 2 - 1) << 6 | (output ? CFG_OUTPUT : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, pull, ma, true);
}

static void
show_pin(const char *name, unsigned pin)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	printf("stfts: GPIO%u (%s): function %u, pull %u, %u mA, %s, in %u out %u\n", pin, name, cfg >> 2 & 0xf, cfg & 3,
	    ((cfg >> 6 & 7) + 1) * 2, cfg & CFG_OUTPUT ? "output" : "input", io & 1, io >> 1 & 1);
}

/* ---- I2C by hand ------------------------------------------------------------------------------- */

#define I2C_ADDRESS             0x49
#define HALF_US                 5               /* 100 kHz */

static unsigned sda = PIN_SDA, scl = PIN_SCL;

/* open drain: a line is either pulled down by us, or let go and pulled up by the bus */
static void
line_low(unsigned pin)
{
	gpio_output(pin, false, PULL_UP, 2);
}

static void
line_release(unsigned pin)
{
	gpio_configure(pin, PULL_UP, 2, false);
}

/* Let SCL go up and wait while the chip holds it down (clock stretching), at most 2 ms. */
static bool
scl_release(void)
{
	line_release(scl);
	for (int i = 0; !gpio_get(scl); i++) {
		if (i > 2000) {
			return false;
		}
		spin_us(1);
	}
	return true;
}

static void
i2c_start(void)
{
	line_release(sda);
	scl_release();
	spin_us(HALF_US);
	line_low(sda);
	spin_us(HALF_US);
	line_low(scl);
}

static void
i2c_stop(void)
{
	line_low(sda);
	spin_us(HALF_US);
	scl_release();
	spin_us(HALF_US);
	line_release(sda);
	spin_us(HALF_US);
}

/* One byte out; true if the chip took it (pulled SDA low on the ninth clock). */
static bool
i2c_put(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		if (byte >> bit & 1) {
			line_release(sda);
		} else {
			line_low(sda);
		}
		spin_us(HALF_US);
		if (!scl_release()) {
			return false;
		}
		spin_us(HALF_US);
		line_low(scl);
	}
	line_release(sda);
	spin_us(HALF_US);
	if (!scl_release()) {
		return false;
	}
	spin_us(HALF_US);
	ack = !gpio_get(sda);
	line_low(scl);
	return ack;
}

static uint8_t
i2c_get(bool more)
{
	uint8_t byte = 0;

	line_release(sda);
	for (int bit = 7; bit >= 0; bit--) {
		spin_us(HALF_US);
		scl_release();
		spin_us(HALF_US);
		byte |= (uint8_t)(gpio_get(sda) << bit);
		line_low(scl);
	}
	if (more) {
		line_low(sda);          /* acknowledge: another byte is wanted */
	}
	spin_us(HALF_US);
	scl_release();
	spin_us(HALF_US);
	line_low(scl);
	line_release(sda);
	return byte;
}

/* A device that was cut off in the middle of a byte can hold SDA down: clock it out of that. */
static void
i2c_recover(void)
{
	line_release(sda);
	scl_release();
	for (int i = 0; i < 9 && !gpio_get(sda); i++) {
		line_low(scl);
		spin_us(HALF_US);
		scl_release();
		spin_us(HALF_US);
	}
	i2c_stop();
}

static bool
i2c_write(const uint8_t *data, uint32_t length)
{
	bool ok;

	i2c_start();
	ok = i2c_put(I2C_ADDRESS << 1);
	for (uint32_t i = 0; ok && i < length; i++) {
		ok = i2c_put(data[i]);
	}
	i2c_stop();
	return ok;
}

/* Write, then read without letting go of the bus between (a repeated start), as the chip wants. */
static bool
i2c_write_read(const uint8_t *out, uint32_t out_length, uint8_t *in, uint32_t in_length)
{
	bool ok;

	i2c_start();
	ok = i2c_put(I2C_ADDRESS << 1);
	for (uint32_t i = 0; ok && i < out_length; i++) {
		ok = i2c_put(out[i]);
	}
	if (ok) {
		i2c_start();
		ok = i2c_put(I2C_ADDRESS << 1 | 1);
	}
	for (uint32_t i = 0; ok && i < in_length; i++) {
		in[i] = i2c_get(i + 1 < in_length);
	}
	i2c_stop();
	return ok;
}

/* Is anybody at the address? */
static bool
i2c_probe(void)
{
	bool ack;

	i2c_start();
	ack = i2c_put(I2C_ADDRESS << 1);
	i2c_stop();
	return ack;
}

/* ---- the analog supply (RPMh) ------------------------------------------------------------------- */

#define CMD_DB_POINTER_PHYS     0x0c3f0000UL    /* AOP's message RAM: at 0xc it says where the command database is */
#define CMD_DB_POINTER          0xc
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define XOB_ENABLE              4               /* an on/off regulator's one register, from its address */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("stfts: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("stfts: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* One register write to RPMh, waited for (the sequence of hakios.goodix.c's rpmh_write). */
static bool
rpmh_write(uint32_t address, uint32_t data)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("stfts: RPMh: the command slots stay busy\n");
			lck_mtx_unlock(&hakios_rsc_lock);
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_CMD_MSGID(0)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
	wr(TCS(TCS_CMD_ADDR(0)), address);
	wr(TCS(TCS_CMD_DATA(0)), data);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 1);
	wr(TCS(TCS_CMD_ENABLE), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/*
 * The analog supply: LDO1 of the PM8150L ("ldoc1"), 1.8 V fixed by the PMIC itself, switched on or off. In MIUI's device
 * tree nothing but the touch screen's "avdd" is on it.
 */
static bool
analog_supply(bool on)
{
	static uint32_t base;

	if (base == 0) {
		vm_offset_t pointer = ml_io_map(CMD_DB_POINTER_PHYS, 0x1000);
		uint32_t db = rd(pointer + CMD_DB_POINTER), size = rd(pointer + CMD_DB_POINTER + 4), found;

		if (db == 0 || size == 0 || size > CMD_DB_SIZE) {
			printf("stfts: RPMh: no command database where AOP says (%#x, %#x bytes)\n", db, size);
			return false;
		}
		cmd_db = ml_io_map_wcomb(db, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
		found = rpmh_address("ldoc1");
		/* regulators' addresses have 4 in bits 19:16 ("VRM"); anything else is not what we are looking for */
		if (found == 0 || (found >> 16 & 0xf) != 4) {
			printf("stfts: RPMh: no regulator \"ldoc1\" (address %#x)\n", found);
			return false;
		}
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		base = found;
		printf("stfts: RPMh: ldoc1 is at %#x; slot status %#x, control %#x\n", base, rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)));
	}
	if (!rpmh_write(base + XOB_ENABLE, on ? 1 : 0)) {
		return false;
	}
	printf("stfts: analog supply %s (PM8150L LDO1, 1.8 V)\n", on ? "on" : "off");
	return true;
}

/* ---- the chip ------------------------------------------------------------------------------------ */

#define FIFO_READ               0x86            /* I2C: read events from the FIFO */
#define EVENT_SIZE              8
#define FIFO_DEPTH              32
#define HW_REG                  0xfa            /* then a 4-byte address: a register of the chip */
#define ADDR_CHIP_ID            0x20000000u
#define CMD_SCAN_MODE           0xa0
#define SCAN_MODE_ACTIVE        0x00
#define EVT_CONTROLLER_READY    0x03
#define EVT_ENTER               0x13
#define EVT_MOTION              0x23
#define EVT_LEAVE               0x33
#define EVT_STATUS              0x43
#define EVT_ERROR               0xf3
#define TOUCH_TYPE_STYLUS       0x03
#define TOUCH_TYPE_HOVER        0x05
#define MAX_X                   1080            /* fts,x-max and fts,y-max of the device tree */
#define MAX_Y                   2340
#define MAX_TOUCHES             10

static uint32_t flip;
static struct {
	bool down;
	uint32_t x, y;
} contact[16];
static struct {
	uint32_t events, touches, readies, errors, statuses, others, failed, last_x, last_y;
} seen;

static bool
fifo_read(uint8_t *events, uint32_t count)
{
	uint8_t command = FIFO_READ;

	return i2c_write_read(&command, 1, events, EVENT_SIZE * count);
}

static bool
read_register(uint32_t address, uint8_t *data, uint32_t length)
{
	uint8_t command[5] = { HW_REG, address >> 24, address >> 16, address >> 8, address };

	return i2c_write_read(command, sizeof(command), data, length);
}

static bool
scan_active(void)
{
	uint8_t command[3] = { CMD_SCAN_MODE, SCAN_MODE_ACTIVE, 0x01 };    /* 0x01: mutual sensing, as Xiaomi's driver has it */

	return i2c_write(command, sizeof(command));
}

static void
release_all(void)
{
	for (uint32_t id = 0; id < MAX_TOUCHES; id++) {
		if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

static void
handle_event(const uint8_t *e)
{
	static unsigned shown, errors_shown;
	uint32_t id = e[1] >> 4, kind = e[1] & 0xf, x, y;

	seen.events++;
	switch (e[0]) {
	case EVT_ENTER:
	case EVT_MOTION:
		if (id >= MAX_TOUCHES || kind == TOUCH_TYPE_STYLUS || kind == TOUCH_TYPE_HOVER) {
			return;
		}
		x = (e[3] & 0xfu) << 8 | e[2];
		y = (uint32_t)e[4] << 4 | e[3] >> 4;
		if (shown < 40) {
			shown++;
			printf("stfts: finger %u %s at %u, %u (of %u x %u)\n", id, e[0] == EVT_ENTER ? "down" : "moves", x, y,
			    MAX_X, MAX_Y);
		}
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		x = x >= MAX_X - 1 ? 65535 : x * 65535 / (MAX_X - 1);
		y = y >= MAX_Y - 1 ? 65535 : y * 65535 / (MAX_Y - 1);
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		if (!contact[id].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
		break;
	case EVT_LEAVE:
		if (id < MAX_TOUCHES && contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
		break;
	case EVT_CONTROLLER_READY:
		/* the chip has started again by itself (a watchdog, ESD): it scans nothing until told to */
		seen.readies++;
		printf("stfts: the chip has reset itself; scanning again\n");
		release_all();
		scan_active();
		break;
	case EVT_ERROR:
		seen.errors++;
		if (errors_shown < 10) {
			errors_shown++;
			printf("stfts: error event %02x %02x %02x %02x %02x %02x %02x %02x\n", e[0], e[1], e[2], e[3], e[4], e[5],
			    e[6], e[7]);
		}
		break;
	case EVT_STATUS:
		seen.statuses++;
		{
			static unsigned statuses_shown;

			if (statuses_shown < 20) {
				statuses_shown++;
				printf("stfts: status event %02x %02x %02x %02x %02x %02x %02x %02x\n", e[0], e[1], e[2], e[3], e[4], e[5],
				    e[6], e[7]);
			}
		}
		break;
	default:
		seen.others++;
		break;
	}
}

/* Everything in the FIFO: the first event says how many more there are (byte 7). */
static void
chip_poll(void)
{
	static uint8_t events[EVENT_SIZE * FIFO_DEPTH];
	uint32_t more;

	if (!fifo_read(events, 1)) {
		seen.failed++;
		i2c_recover();
		return;
	}
	if (events[0] == 0) {
		return;
	}
	more = events[7] & 0x1f;
	if (more > FIFO_DEPTH - 1) {
		more = FIFO_DEPTH - 1;
	}
	if (more > 0 && !fifo_read(events + EVENT_SIZE, more)) {
		seen.failed++;
		i2c_recover();
		more = 0;
	}
	for (uint32_t i = 0; i <= more && events[i * EVENT_SIZE] != 0; i++) {
		handle_event(events + i * EVENT_SIZE);
	}
}

/* Reset the chip and wait for it to say it has started: up to a second. */
static bool
chip_reset(void)
{
	uint8_t event[EVENT_SIZE];

	gpio_output(PIN_RESET, false, PULL_UP, 8);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	sleep_ms(10);
	for (int i = 0; i < 100; i++) {
		if (fifo_read(event, 1) && event[0] == EVT_CONTROLLER_READY) {
			printf("stfts: the chip has started (%d ms after reset)\n", 20 + 10 * i);
			return true;
		}
		sleep_ms(10);
	}
	printf("stfts: no \"controller ready\" from the chip within a second of its reset\n");
	return false;
}

/* Does the chip answer on the pins as they are named, or the other way round? */
static bool
find_chip(void)
{
	for (int swapped = 0; swapped < 2; swapped++) {
		sda = swapped ? PIN_SCL : PIN_SDA;
		scl = swapped ? PIN_SDA : PIN_SCL;
		i2c_recover();
		if (i2c_probe()) {
			printf("stfts: the chip answers at 0x%x, SDA on GPIO%u and SCL on GPIO%u\n", I2C_ADDRESS, sda, scl);
			return true;
		}
	}
	sda = PIN_SDA;
	scl = PIN_SCL;
	return false;
}

/*
 * Both of the chip's supplies off and on again, as a power-off does: after MIUI (or TWRP) the phone restarts warm, the
 * supplies are never let down, and the chip comes up from a reset by its pin with Android's leftovers in it -- it answers,
 * takes the scan mode, and reports a few touches at the edges and then nothing (2026-11-05, logs/104324; only a full
 * power-off helped). The pins it is wired to are pulled down meanwhile, so that it is not fed through them. touch_cycle=0:
 * not done; touch_off_ms: how long the supplies stay off (300).
 */
static void
power_cycle(void)
{
	uint32_t cycle = 1, off_ms = 300;

	PE_parse_boot_argn("touch_cycle", &cycle, sizeof(cycle));
	PE_parse_boot_argn("touch_off_ms", &off_ms, sizeof(off_ms));
	if (!cycle) {
		return;
	}
	if (off_ms < 10 || off_ms > 5000) {
		off_ms = 300;
	}
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	gpio_configure(PIN_SDA, PULL_DOWN, 2, false);
	gpio_configure(PIN_SCL, PULL_DOWN, 2, false);
	gpio_configure(PIN_INT, PULL_DOWN, 2, false);
	if (!analog_supply(false)) {
		printf("stfts: power cycle: the analog supply could not be switched off\n");
	}
	gpio_output(PIN_VDDIO, false, PULL_NONE, 2);
	sleep_ms(off_ms);
	printf("stfts: power cycle: supplies were off for %u ms (interrupt line %s while off)\n", off_ms,
	    gpio_get(PIN_INT) ? "high" : "low");
	gpio_output(PIN_VDDIO, true, PULL_NONE, 2);
	sleep_ms(5);                    /* the regulator's startup-delay-us in the device tree: 4000 */
	analog_supply(true);
	sleep_ms(15);
	gpio_configure(PIN_SDA, PULL_UP, 2, false);
	gpio_configure(PIN_SCL, PULL_UP, 2, false);
	gpio_configure(PIN_INT, PULL_UP, 8, false);
}

static bool
bring_up(void)
{
	uint8_t id[6] = { 0 };          /* 0x20000000: the chip's id, two bytes, then (0x20000004) its firmware's version */

	show_pin("SDA", PIN_SDA);
	show_pin("SCL", PIN_SCL);
	show_pin("reset", PIN_RESET);
	show_pin("interrupt", PIN_INT);
	show_pin("I/O supply", PIN_VDDIO);
	power_cycle();
	gpio_configure(PIN_INT, PULL_UP, 8, false);
	/*
	 * Always a reset by the pin, as Xiaomi's driver does at its start: low 10 ms, then high. (2026-11-04: with the pin
	 * left high by a run before, the chip answered, took the system reset and the scan mode, and reported no touch at all.)
	 */
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	sleep_ms(50);
	if (!find_chip()) {
		printf("stfts: no answer at 0x%x: turning the supplies on\n", I2C_ADDRESS);
		gpio_output(PIN_RESET, false, PULL_UP, 8);
		if (!(rd(GPIO_CFG(PIN_VDDIO)) & CFG_OUTPUT) || !(rd(GPIO_IO(PIN_VDDIO)) & 2)) {
			gpio_output(PIN_VDDIO, true, PULL_NONE, 2);
			printf("stfts: I/O supply switched on (GPIO%u)\n", PIN_VDDIO);
		}
		sleep_ms(1);
		if (!analog_supply(true)) {
			return false;
		}
		sleep_ms(15);
		gpio_set(PIN_RESET, true);
		sleep_ms(50);
		if (!find_chip()) {
			printf("stfts: still no answer at 0x%x with the supplies on\n", I2C_ADDRESS);
			return false;
		}
	}
	/*
	 * The analog supply asked for every time, not only when the chip is silent: the chip runs on its I/O supply alone and
	 * answers, but senses nothing without this one (2026-11-04: answering, taking the scan mode, no touch reported).
	 */
	if (!analog_supply(true)) {
		printf("stfts: the analog supply could not be asked for\n");
	} else {
		printf("stfts: the analog supply (ldoc1) asked for\n");
	}
	sleep_ms(15);
	if (!chip_reset()) {
		return false;
	}
	if (!read_register(ADDR_CHIP_ID, id, sizeof(id))) {
		printf("stfts: the chip's id could not be read\n");
		return false;
	}
	printf("stfts: chip id %02x %02x, firmware %02x%02x%s\n", id[0], id[1], id[5], id[4],
	    id[0] == 0x36 && id[1] == 0x39 ? " (FTM5)" : " (NOT the FTM5 that was expected)");
	if (!scan_active()) {
		printf("stfts: the chip did not take the scan mode command\n");
		return false;
	}
	{
		/* the charger mode (Xiaomi's setFeatures(FEAT_SEL_CHARGER, 1): A2 02 01): the phone is on USB while it runs */
		uint8_t charger[3] = { 0xa2, 0x02, 0x01 };

		printf("stfts: charger mode %s\n", i2c_write(charger, sizeof(charger)) ? "on" : "NOT TAKEN");
	}
	sleep_ms(10);
	chip_poll();                    /* whatever the commands left in the FIFO */
	return true;
}

static void
stfts_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* the FIFO read every 80 ms even with the line high, in case it is missed */
	nanoseconds_to_absolutetime(NSEC_PER_USEC, &ticks_per_us);

	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	if (!bring_up()) {
		printf("stfts: no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("stfts: touch screen ready; the interrupt line is looked at every %u us\n", poll_us);
	for (unsigned tick = 1;; tick++) {
		/* the line stays low while events wait; a few reads in a row drain a full FIFO */
		for (int i = 0; i < 4 && !gpio_get(PIN_INT); i++) {
			idle = 0;
			chip_poll();
		}
		if (++idle >= idle_looks) {
			idle = 0;
			chip_poll();
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("stfts: so far %u events, %u finger positions, the last at %u, %u; %u resets of the chip, %u errors, "
			    "%u status events, %u others, %u reads that failed; interrupt line %s\n", seen.events, seen.touches,
			    seen.last_x, seen.last_y, seen.readies, seen.errors, seen.statuses, seen.others, seen.failed,
			    gpio_get(PIN_INT) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}

void
hakios_stfts_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(stfts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
