/*
 * The SLIMbus of the Xiaomi Mi 9 (cepheus, SM8150), and the codec behind it: step 1 of the earpiece. The earpiece (the
 * receiver at the top) is driven by the EAR output of a Qualcomm WCD9340 ("Tavil"), whose registers and samples go over
 * the SoC's SLIMbus; the bus belongs to the audio DSP (it is its "master"), and the application processor has a
 * "non-ported generic device" on it (NGD), through which it sends and receives messages. Before the NGD's registers may be
 * touched, the DSP is asked over QMI to power the bus (Linux: drivers/slimbus/slim-msm.c msm_slim_qmi_init() and
 * msm_slim_qmi_power_request(), slim-msm-ngd.c ngd_slim_power_up(); Xiaomi's kernel, LineageOS android_kernel_xiaomi_sm8150).
 *
 * Every number below is the phone's, from its device tree and Android's files:
 *
 *   the NGD    slim@171c0000 (qcom,slim-ngd, cell-index 1: its registers at 0x171c0000 + 0x1000, NGD_BASE_V2(1)), SPI 163
 *   the DSP's  QMI service 0x301 (SLIMBUS_QMI_SVC_ID), version 1, instance 0: the DSP tells of it over QRTR (node 5, port
 *   service    12 on this phone); "select instance" (0x20): instance 0 (cell-index >> 1), mode MASTER (the DSP's role);
 *              "power" (0x21): ACTIVE (2), INACTIVE (1)
 *   the codec  tavil_codec: elemental addresses 00 01 50 02 17 02 (its generic device) and 00 00 50 02 17 02 (its
 *              interface device); reset GPIO 143 (south; msm_cdc_pinctrl@143, active high), its MCLK 9.6 MHz from the
 *              PMIC's LN_BB_CLK2 (audio_ext_clk_lnbb), supplies pm8150_s4 (1.8 V) and pm8150l_bob
 *   Android    the earpiece ("handset"): SLIM_0_RX, one channel -> RX0 -> RX INT0 -> class H -> EAR PA, +6 dB
 *              (mixer_paths_overlay_static.xml)
 *
 * Nothing here is done unless a boot-arg asks for it, and only once the DSP is up (adsp=1):
 *
 *   slim=1   the DSP asked for the bus (select instance, power ACTIVE); the NGD's version and state read (nothing
 *            written to it); the codec's reset pin looked at; then the power given back (INACTIVE)
 *   slim=2   ... and the NGD enabled as ngd_slim_power_up() enables it, without the BAM message queues (messages through
 *            its registers: NGD_TX_MSG, NGD_RX_MSG), its interrupts unmasked (they are only read: polled); the manager's
 *            "master capability" waited for and answered with "report satellite" (ngd_slim_rx_msgq_thread(): the magic
 *            D9 C5, version 1, protocol 1), every message that comes for two seconds more said; then the NGD disabled
 *            again and the power given back
 *   slim=3   ... and, with the NGD up, the codec: its MCLK on (RPMh "lnbclka2", the PMIC's LN_BB_CLK2: clk-rpmh's VRM
 *            clock, 1 at its address + 4), its reset released as wcd9xxx_reset() releases it (low 20 ms, then high,
 *            8 mA, pull-down: msm_cdc_pinctrl's active state), the logical addresses of its generic and interface
 *            devices asked of the manager (SLIM_USR_MC_ADDR_QUERY, as ngd_get_laddr(); again every 50 ms for 2 s while
 *            it enumerates), and its chip id read (page 0 written to its page register, REQUEST_VALUE of 0x21..0x24 at
 *            0x800 + register: wcd9xxx_slim_read_device(), wcd934x_get_cdc_info(); Tavil's major id 0x108). Nothing else is
 *            written to the codec; at the end it is put back into reset and its MCLK switched off.
 *   slim=4   ... and the codec brought up and its earpiece path powered, with no samples yet (silence): Linux's
 *            wcd934x_bring_up() (mfd/wcd934x.c), its MCLK inside it (wcd934x_enable_ana_bias_and_sysclk(): the bias
 *            and its pre-charge, the external clock's buffer divided by 2 -- LN_BB_CLK2's 19.2 MHz to 9.6 --, the
 *            frame counter, SIDO from the RCO), then the path DAPM powers for the "handset" of Android's mixer paths
 *            (RX0 -> RX INT0 -> class H -> EAR PA): the RX bias, INT0's clock and its DSM/DEM clock, INT0's first input
 *            RX0, the DEM input CLSH_DSM_OUT with its look-ahead delay, the EAR DAC's class H (wcd-clsh-v2.c,
 *            wcd_clsh_state_ear(): the CRC clock, buck and flyback, the flyback's currents), the EAR PA, 5 ms, INT0
 *            unmuted (tavil_codec_enable_ear_pa()); held for two seconds, the EAR's status looked at; then all of it
 *            powered down in the reverse order (the PA first), the codec back into reset. Every write read back.
 *
 * Checked: compiled. On the phone (2026-11-04, logs/160631-run.log, adsp=1 slim=1): the DSP tells of its service at node 5
 * port 12; "select instance" and "power: ACTIVE" answered with success in 1 and 10 ms; the controller's version word
 * 0x2020000 (0x202); the NGD readable once the bus is powered: cfg 0 (not enabled), status 0x40c (message queues
 * supported, no logical address of ours yet), interrupts off; the codec's reset an input with a pull-down: held in reset.
 * "power: INACTIVE" answered too.
 * slim=2 (logs/161154): enabled, the NGD's status 0x40d; the manager's capability (c7 00 ff d9 c5 01 01) within 1 ms, our
 * "report satellite" sent in 134 us, no NACK; the status then 0x240e -- a logical address is ours. Disabled again after.
 * slim=3 (logs/162043): RPMh has "lnbclka2" at 0x42f00 (the clocks it knows: lnbclka1..3, rfclka1..3, divclka1..2,
 * divclkc1), the MCLK on; 51 ms after the reset the manager knows the codec's interface device (0xce) and its generic
 * device (0xcf) -- before that, address replies with an elemental address of zeroes; the chip id 01 00 08 01: major
 * 0x108 (Tavil), minor 1 (1.1). Back into reset and the MCLK off after.
 * slim=4 (logs/162759): the bring-up, the MCLK inside the codec and the earpiece path: 48 registers changed, every one
 * read back as written; with the path on ANA_EAR 0x80 (the PA on), the RX supplies 0xc1 (bias, flyback, buck), INT0's
 * path 0x24 (its clock, 48 kHz, unmuted), EAR_STATUS 0x08 as before, the same two seconds later; all of it off again.
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

static uint32_t level;

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

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- QMI over QRTR (Linux's qmi_interface.c, as far as two requests need it) --------------------------------------- */

#define SLIM_QMI_SERVICE        0x301
#define SLIM_QMI_VERSION        1
#define SLIM_QMI_INSTANCE       0
#define QMI_SELECT_INSTANCE     0x0020
#define QMI_POWER               0x0021
#define SLIM_MODE_MASTER        2               /* the DSP's role: Linux's msm_slim_qmi_init(dev, false) */
#define SLIM_PM_INACTIVE        1
#define SLIM_PM_ACTIVE          2

static uint32_t our_port, dsp_node, dsp_port;
static volatile bool answered;
static volatile uint16_t answer_txn, answer_msg, answer_result, answer_error;
static uint16_t next_txn = 1;

/* On the link's thread: a response (type 2) with its result TLV (0x02: result, error) */
static void
qmi_receive(__unused uint32_t from_node, __unused uint32_t from_port, const uint8_t *data, uint32_t length)
{
	uint32_t at = 7, size;

	if (length < 7 || data[0] != 2) {
		return;
	}
	size = (uint32_t)(data[5] | data[6] << 8);
	answer_result = answer_error = 0xffff;
	while (at + 3 <= length && at + 3 <= 7 + size) {
		uint8_t type = data[at];
		uint32_t tlv = (uint32_t)(data[at + 1] | data[at + 2] << 8);

		if (at + 3 + tlv > length) {
			break;
		}
		if (type == 0x02 && tlv >= 4) {
			answer_result = (uint16_t)(data[at + 3] | data[at + 4] << 8);
			answer_error = (uint16_t)(data[at + 5] | data[at + 6] << 8);
		}
		at += 3 + tlv;
	}
	answer_txn = (uint16_t)(data[1] | data[2] << 8);
	answer_msg = (uint16_t)(data[3] | data[4] << 8);
	__asm__ volatile ("dmb ish" ::: "memory");
	answered = true;
}

/* A request of 32-bit TLVs (type, value), its response waited for: true if the result is "success". */
static bool
qmi_request(const char *what, uint16_t msg, const uint8_t *types, const uint32_t *values, unsigned count)
{
	uint8_t packet[64];
	uint32_t length = 7;
	uint16_t txn = next_txn++;
	uint64_t began;

	packet[0] = 0;                          /* a request */
	packet[1] = (uint8_t)txn;
	packet[2] = (uint8_t)(txn >> 8);
	packet[3] = (uint8_t)msg;
	packet[4] = (uint8_t)(msg >> 8);
	for (unsigned i = 0; i < count; i++) {
		packet[length++] = types[i];
		packet[length++] = 4;
		packet[length++] = 0;
		for (unsigned b = 0; b < 4; b++) {
			packet[length++] = (uint8_t)(values[i] >> (8 * b));
		}
	}
	packet[5] = (uint8_t)(length - 7);
	packet[6] = (uint8_t)((length - 7) >> 8);
	answered = false;
	say("slim: QMI: %s (message %#x, transaction %u) to node %u port %u\n", what, msg, txn, dsp_node, dsp_port);
	if (!hakios_adsp_qrtr_send(our_port, dsp_node, dsp_port, packet, length)) {
		say("slim: QMI: it cannot be sent (the link is not up?)\n");
		return false;
	}
	began = now_us();
	while (!answered && now_us() - began < 2000000) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (!answered) {
		say("slim: QMI: %s: NO ANSWER within 2 s\n", what);
		return false;
	}
	say("slim: QMI: %s: answer to transaction %u (message %#x) after %llu ms: result %u, error %u%s\n", what, answer_txn,
	    answer_msg, (now_us() - began) / 1000, answer_result, answer_error, answer_result == 0 ? "" : " -- REFUSED");
	return answer_txn == txn && answer_msg == msg && answer_result == 0;
}

static bool powered;

static bool
bus_power(bool on)
{
	const uint8_t type[1] = { 0x01 };
	const uint32_t value[1] = { on ? SLIM_PM_ACTIVE : SLIM_PM_INACTIVE };

	if (!qmi_request(on ? "power: ACTIVE" : "power: INACTIVE", QMI_POWER, type, value, 1)) {
		return false;
	}
	powered = on;
	return true;
}

/* ---- the NGD's registers (slim-msm-ngd.c) ----------------------------------------------------------------------- */

#define SLIM_PHYS               0x171c0000UL
#define SLIM_SIZE               0x2c000UL
#define NGD_OFFSET              0x1000UL        /* NGD_BASE_V2(cell-index 1) */
#define NGD_CFG                 0x00
#define NGD_STATUS              0x04            /* [1] a logical address is ours */
#define NGD_RX_MSGQ_CFG         0x08
#define NGD_INT_EN              0x10
#define NGD_INT_STAT            0x14
#define NGD_INT_CLR             0x18
#define NGD_TX_MSG              0x30
#define NGD_RX_MSG              0x70
#define NGD_CFG_ENABLE          1u
#define INT_TX_NACKED_2         (1u << 25)
#define INT_MSG_BUF_CONTE       (1u << 26)
#define INT_MSG_TX_INVAL        (1u << 27)
#define INT_IE_VE_CHG           (1u << 28)
#define INT_DEV_ERR             (1u << 29)
#define INT_RX_MSG_RCVD         (1u << 30)
#define INT_TX_MSG_SENT         (1u << 31)
#define INT_RECFG_DONE          (1u << 24)
#define RX_MSGQ_TIMEOUT         0x10000u

/* the messages (include/linux/slimbus/slimbus.h, slim-msm.h) */
#define MT_CORE                 0x0
#define MT_DEST_REFERRED_USER   0x2
#define MT_SRC_REFERRED_USER    0x6
#define MC_MASTER_CAPABILITY    0x00            /* (user) */
#define MC_REPORT_SATELLITE     0x01            /* (user) */
#define MC_GENERIC_ACK          0x25            /* (user) */
#define MC_REPORT_PRESENT       0x01            /* (core) */
#define LA_MANAGER              0xff
#define MC_ADDR_QUERY           0x0d            /* (user) */
#define MC_ADDR_REPLY           0x0e            /* (user) */
#define MC_REQUEST_VALUE        0x60            /* (core) */
#define MC_REPLY_VALUE          0x64
#define MC_CHANGE_VALUE         0x68
#define WCD_REGISTERS           0x800           /* WCD9XXX_REGISTER_START_OFFSET */
#define TAVIL_MAJOR             0x0108
#define FIRST_WORD(length, mt, mc, dt, la) ((length) | (mt) << 5 | (mc) << 8 | (dt) << 15 | (uint32_t)(la) << 16)

#define TLMM_SOUTH_PHYS         0x03d00000UL
#define PIN_CODEC_RESET         143

static vm_offset_t slim_regs, ngd;
static bool ngd_enabled;

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* A message the NGD has received, if one waits: its words (the length in the low five bits of the first byte). */
static bool
ngd_receive(uint8_t *bytes, uint32_t *length)
{
	uint32_t stat = rd(ngd + NGD_INT_STAT), words[10];

	if (!(stat & INT_RX_MSG_RCVD)) {
		return false;
	}
	words[0] = rd(ngd + NGD_RX_MSG);
	*length = words[0] & 0x1f;
	for (uint32_t i = 1; i < ((*length + 3) >> 2) && i < 10; i++) {
		words[i] = rd(ngd + NGD_RX_MSG + 4 * i);
	}
	wr(ngd + NGD_INT_CLR, INT_RX_MSG_RCVD);
	memcpy(bytes, words, *length > 40 ? 40 : *length);
	return true;
}

static void
say_message(const char *when, const uint8_t *m, uint32_t length)
{
	char hex[3 * 40 + 1];
	int at = 0;

	for (uint32_t i = 0; i < length && i < 40; i++) {
		at += snprintf(hex + at, sizeof(hex) - (size_t)at, " %02x", m[i]);
	}
	hex[at] = 0;
	say("slim: %s: a message of %u bytes, type %u, code %#x:%s\n", when, length, (m[0] >> 5) & 7, m[1], hex);
}

/* A message sent through the NGD's registers; "sent" (or a NACK, an error) waited for. */
static bool
ngd_send(const char *what, const uint8_t *bytes, uint32_t length)
{
	uint32_t words[10] = { 0 }, stat = 0;
	uint64_t began;

	memcpy(words, bytes, length);
	wr(ngd + NGD_INT_CLR, INT_TX_MSG_SENT | INT_TX_NACKED_2 | INT_MSG_TX_INVAL | INT_DEV_ERR | INT_MSG_BUF_CONTE);
	for (uint32_t i = 0; i < ((length + 3) >> 2); i++) {
		wr(ngd + NGD_TX_MSG + 4 * i, words[i]);
	}
	began = now_us();
	while (now_us() - began < 1000000) {
		stat = rd(ngd + NGD_INT_STAT);
		if (stat & (INT_TX_MSG_SENT | INT_TX_NACKED_2 | INT_MSG_TX_INVAL | INT_DEV_ERR | INT_MSG_BUF_CONTE)) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	wr(ngd + NGD_INT_CLR, stat & (INT_TX_MSG_SENT | INT_TX_NACKED_2 | INT_MSG_TX_INVAL | INT_DEV_ERR | INT_MSG_BUF_CONTE));
	if ((stat & INT_TX_MSG_SENT) && !(stat & (INT_TX_NACKED_2 | INT_MSG_TX_INVAL | INT_DEV_ERR | INT_MSG_BUF_CONTE)) &&
	    strncmp(what, "report", 6) != 0) {
		return true;            /* (only what is not sent, and the capability's answer, is said: the codec's are hundreds) */
	}
	say("slim: %s: %s after %llu us (int stat %#x)\n", what, (stat & INT_TX_MSG_SENT) && !(stat & ~INT_TX_MSG_SENT &
	    (INT_TX_NACKED_2 | INT_MSG_TX_INVAL | INT_DEV_ERR | INT_MSG_BUF_CONTE)) ? "SENT" : (stat & INT_TX_NACKED_2) ? "NACKED" :
	    (stat & INT_MSG_TX_INVAL) ? "INVALID" : (stat & INT_DEV_ERR) ? "a device error" : (stat & INT_MSG_BUF_CONTE) ?
	    "a buffer contention" : "NOT SENT", now_us() - began, stat);
	return (stat & INT_TX_MSG_SENT) && !(stat & (INT_TX_NACKED_2 | INT_MSG_TX_INVAL | INT_DEV_ERR | INT_MSG_BUF_CONTE));
}

/* slim=2: ngd_slim_power_up() without the BAM, and the capability exchange */
static void
ngd_up(void)
{
	uint8_t m[40];
	uint32_t length = 0;
	uint64_t began;
	bool capable = false;

	say("slim: the NGD is enabled (interrupts unmasked, read by polling; messages through its registers)\n");
	wr(ngd + NGD_INT_EN, INT_TX_NACKED_2 | INT_MSG_BUF_CONTE | INT_MSG_TX_INVAL | INT_IE_VE_CHG | INT_DEV_ERR | INT_TX_MSG_SENT |
	    INT_RX_MSG_RCVD | INT_RECFG_DONE);
	wr(ngd + NGD_RX_MSGQ_CFG, rd(ngd + NGD_RX_MSGQ_CFG) | RX_MSGQ_TIMEOUT);
	wr(ngd + NGD_CFG, NGD_CFG_ENABLE);
	ngd_enabled = true;
	say("slim: the NGD now: cfg %#x, status %#x, int stat %#x\n", rd(ngd + NGD_CFG), rd(ngd + NGD_STATUS), rd(ngd + NGD_INT_STAT));
	began = now_us();
	while (now_us() - began < 3000000 && !capable) {
		if (ngd_receive(m, &length)) {
			say_message("from the bus", m, length);
			if (((m[0] >> 5) & 7) == MT_SRC_REFERRED_USER && m[1] == MC_MASTER_CAPABILITY) {
				/* "report satellite" to the manager: length 7, D9 C5, version 1, protocol 1 (ngd_slim_rx_msgq_thread) */
				uint8_t reply[7];
				uint32_t first = FIRST_WORD(7, MT_SRC_REFERRED_USER, MC_REPORT_SATELLITE, 0, LA_MANAGER);

				say("slim: THE MANAGER TELLS ITS CAPABILITY after %llu ms: answered with \"report satellite\"\n",
				    (now_us() - began) / 1000);
				memcpy(reply, &first, 3);
				reply[3] = 0xd9;
				reply[4] = 0xc5;
				reply[5] = 1;
				reply[6] = 1;
				capable = ngd_send("report satellite", reply, sizeof(reply));
			}
			continue;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (!capable) {
		say("slim: NO CAPABILITY EXCHANGE within 3 s (status %#x, int stat %#x)\n", rd(ngd + NGD_STATUS), rd(ngd + NGD_INT_STAT));
	}
	began = now_us();
	while (now_us() - began < 2000000) {
		if (ngd_receive(m, &length)) {
			say_message("then", m, length);
			continue;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	say("slim: after it: the NGD's status %#x%s, int stat %#x\n", rd(ngd + NGD_STATUS), (rd(ngd + NGD_STATUS) & 2) ?
	    " (A LOGICAL ADDRESS IS OURS)" : "", rd(ngd + NGD_INT_STAT));
}

/* ---- slim=3: the codec ----------------------------------------------------------------------------------------- */

extern lck_mtx_t hakios_rsc_lock;               /* RPMh's sequencer, shared with the other drivers that ask it */
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define CMD_DB_POINTER_PHYS     0x0c3f0000UL    /* AOP's message RAM: at 0xc, where the command database is */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2"; command slot set 0, active */
#define TCS(reg)                (rsc + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_WAITED      (0x10008u | 0x100u)
#define VRM_ENABLE              4

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

static vm_offset_t rsc;

static uint32_t
rpmh_address(const char *name)
{
	vm_offset_t pointer = ml_io_map(CMD_DB_POINTER_PHYS, 0x1000);
	uint32_t at = rd(pointer + 0xc);
	const struct cmd_db_header *db;
	char id[8] = { 0 };

	if (at == 0) {
		return 0;
	}
	db = (const struct cmd_db_header *)ml_io_map_wcomb(at, CMD_DB_SIZE);
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		say("slim: RPMh: no command database at %#x (magic %02x %02x %02x %02x)\n", at, db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	memcpy(id, name, strnlen(name, sizeof(id)));      /* (8 letters, no terminating zero: "lnbclka2" fills it) */
	{
		/* (once: the clocks RPMh knows, for when a name is not what this thinks) */
		static bool told;
		char line[260];
		int used = 0;

		for (unsigned sl = 0; sl < 8 && !told; sl++) {
			const struct cmd_db_slave *slave = &db->slave[sl];
			const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

			if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
				continue;
			}
			for (unsigned i = 0; i < slave->count; i++) {
				if (strnstr(entry[i].id, "clk", 8) != NULL && used < (int)sizeof(line) - 24) {
					used += snprintf(line + used, sizeof(line) - (size_t)used, " %.8s=%#x", entry[i].id, entry[i].address);
				}
			}
		}
		if (!told) {
			told = true;
			line[used] = 0;
			say("slim: RPMh's clocks (command database at %#x):%s\n", at, line);
		}
	}
	for (unsigned sl = 0; sl < 8; sl++) {
		const struct cmd_db_slave *slave = &db->slave[sl];
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

/* One write to RPMh through the active command slot set, waited for (hakios.stfts.c's rpmh_write) */
static bool
rpmh_write(uint32_t address, uint32_t data)
{
	uint32_t irq_enable;
	bool done = false;

	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, 0x10000);
	}
	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			lck_mtx_unlock(&hakios_rsc_lock);
			say("slim: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_CMD_MSGID(0)), MSGID_WRITE_WAITED);
	wr(TCS(TCS_CMD_ADDR(0)), address);
	wr(TCS(TCS_CMD_DATA(0)), data);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 1);
	wr(TCS(TCS_CMD_ENABLE), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	for (int i = 0; i < 2000 && !done; i++) {
		done = (rd(TCS(TCS_IRQ_STATUS)) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

static uint32_t mclk_address;
static vm_offset_t tlmm_south;
static uint32_t reset_cfg, reset_io;
static bool mclk_on, reset_released;
static uint8_t next_tid = 1;

static const uint8_t ea_generic[6] = { 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 };    /* dt elemental-addr of tavil_codec */
static const uint8_t ea_interface[6] = { 0x00, 0x00, 0x50, 0x02, 0x17, 0x02 };  /* qcom,cdc-slim-ifd-elemental-addr */

static bool
mclk(bool on)
{
	if (mclk_address == 0) {
		mclk_address = rpmh_address("lnbclka2");
		say("slim: RPMh has \"lnbclka2\" (LN_BB_CLK2, the codec's MCLK) at %#x\n", mclk_address);
		if (mclk_address == 0) {
			return false;
		}
	}
	if (!rpmh_write(mclk_address + VRM_ENABLE, on ? 1 : 0)) {
		say("slim: RPMh has not said done to the MCLK %s\n", on ? "on" : "off");
		return false;
	}
	mclk_on = on;
	say("slim: the codec's MCLK %s (RPMh %#x = %u)\n", on ? "ON" : "off", mclk_address + VRM_ENABLE, on ? 1 : 0);
	return true;
}

/* A message for this transaction (a reply that carries its tid in byte 3) waited for, other messages said. */
static bool
wait_reply(uint8_t mt, uint8_t mc, uint8_t tid, uint8_t *m, uint32_t *length, uint32_t ms)
{
	uint64_t began = now_us();

	while (now_us() - began < ms * 1000ull) {
		if (ngd_receive(m, length)) {
			if (((m[0] >> 5) & 7) == mt && m[1] == mc && *length > 3 && m[3] == tid) {
				return true;
			}
			say_message("meanwhile", m, *length);
			continue;
		}
		delay_for_interval(200, NSEC_PER_USEC);
	}
	return false;
}

/* The manager's logical address for an elemental address; 0xff: none (yet). */
static uint8_t
address_query(const uint8_t ea[6])
{
	uint8_t q[10], m[40];
	uint32_t first = FIRST_WORD(10, MT_DEST_REFERRED_USER, MC_ADDR_QUERY, 0, LA_MANAGER), length = 0;
	uint8_t tid = next_tid++;

	memcpy(q, &first, 3);
	q[3] = tid;
	memcpy(q + 4, ea, 6);
	if (!ngd_send("address query", q, sizeof(q))) {
		return 0xff;
	}
	if (!wait_reply(MT_SRC_REFERRED_USER, MC_ADDR_REPLY, tid, m, &length, 500)) {
		say("slim: no address reply within 500 ms\n");
		return 0xff;
	}
	{
		static const uint8_t none[6] = { 0, 0, 0, 0, 0, 0 };
		static unsigned told;

		/* (Linux's ngd_slim_rx: an elemental address of zeroes in the reply is "nobody of that address", not address 0) */
		if (length < 11 || memcmp(m + 4, none, 6) == 0) {
			if (told++ < 2) {
				say_message("the address reply (not there yet)", m, length);
			}
			return 0xff;
		}
	}
	say_message("the address reply", m, length);
	return m[10];
}

static bool
codec_write(uint8_t la, uint16_t offset, uint8_t value)
{
	uint8_t w[6];
	uint16_t ec = (uint16_t)(0 | 1u << 3 | (offset & 0xfff) << 4);        /* one byte: slice code 0 */
	uint32_t first = FIRST_WORD(6, MT_CORE, MC_CHANGE_VALUE, 0, la);

	memcpy(w, &first, 3);
	w[3] = (uint8_t)ec;
	w[4] = (uint8_t)(ec >> 8);
	w[5] = value;
	return ngd_send("a codec register written", w, sizeof(w));
}

/* `count' (1..4) bytes from `offset' of the device at `la' */
static bool
codec_read(uint8_t la, uint16_t offset, uint8_t *bytes, uint32_t count)
{
	static const uint8_t code[5] = { 0, 0, 1, 2, 3 };
	uint8_t r[6], m[40];
	uint8_t tid = next_tid++;
	uint16_t ec = (uint16_t)(code[count] | 1u << 3 | (offset & 0xfff) << 4);
	uint32_t first = FIRST_WORD(6, MT_CORE, MC_REQUEST_VALUE, 0, la), length = 0;

	memcpy(r, &first, 3);
	r[3] = tid;
	r[4] = (uint8_t)ec;
	r[5] = (uint8_t)(ec >> 8);
	if (!ngd_send("a codec register asked for", r, sizeof(r))) {
		return false;
	}
	if (!wait_reply(MT_CORE, MC_REPLY_VALUE, tid, m, &length, 500)) {
		say("slim: no reply value within 500 ms\n");
		return false;
	}
	if (length < 4 + count) {
		say_message("a short reply", m, length);
		return false;
	}
	memcpy(bytes, m + 4, count);
	return true;
}

/* ---- slim=4: the codec's registers, its bring-up and the earpiece path --------------------------------------------- */

/* Tavil's registers (include/linux/mfd/wcd934x/registers.h): page in the high byte, written to the page register (0) */
#define R_CODEC_RPM_CLK_GATE            0x0002
#define R_CODEC_RPM_CLK_MCLK_CFG        0x0003
#define R_CODEC_RPM_RST_CTL             0x0009
#define R_CODEC_RPM_PWR_CDC_DIG_HM_CTL  0x0011
#define R_ANA_BIAS                      0x0601
#define R_ANA_RCO                       0x0603
#define R_ANA_RX_SUPPLIES               0x0608
#define R_ANA_EAR                       0x060a
#define R_RX_BIAS_FLYB_BUFF             0x06c7
#define R_EAR_STATUS_REG                0x06e6
#define R_CLK_SYS_MCLK_PRG              0x0711
#define R_SIDO_NEW_VOUT_A_STARTUP       0x071b
#define R_SIDO_NEW_VOUT_D_STARTUP       0x071c
#define R_HPH_NEW_INT_HPH_TIMER1        0x073a
#define R_CDC_RX0_RX_PATH_CTL           0x0b41
#define R_CDC_RX0_RX_PATH_CFG0          0x0b42
#define R_CDC_RX0_RX_VOL_CTL            0x0b45
#define R_CDC_RX0_RX_PATH_MIX_CTL       0x0b46
#define R_CDC_RX0_RX_PATH_SEC0          0x0b49
#define R_CDC_RX0_RX_PATH_DSMDEM_CTL    0x0b53
#define R_CDC_CLSH_CRC                  0x0c01
#define R_CDC_RX_INP_MUX_RX_INT0_CFG0   0x0d01
#define R_CDC_CLK_RST_CTRL_MCLK_CONTROL 0x0d41
#define R_CDC_CLK_RST_CTRL_FS_CNT_CONTROL 0x0d42

static uint8_t codec_la = 0xff;
static int codec_page = -1;
static unsigned codec_writes, codec_mismatches;
static bool codec_failed;

static bool
codec_page_for(uint16_t reg)
{
	uint8_t page = (uint8_t)(reg >> 8);

	if (codec_page == page) {
		return true;
	}
	if (!codec_write(codec_la, WCD_REGISTERS + 0x00, page)) {
		codec_failed = true;
		return false;
	}
	codec_page = page;
	return true;
}

static bool
reg_read(uint16_t reg, uint8_t *value)
{
	if (codec_failed || !codec_page_for(reg) || !codec_read(codec_la, WCD_REGISTERS + (reg & 0xff), value, 1)) {
		codec_failed = true;
		say("slim: codec register %#06x cannot be read\n", reg);
		return false;
	}
	return true;
}

/* the bits of `mask' to `value', read back (bits that the codec clears by itself may not, `volatile_mask') */
static bool
reg_update(uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0, want, back = 0;

	if (!reg_read(reg, &now)) {
		return false;
	}
	want = (uint8_t)((now & ~mask) | (value & mask));
	if (want != now) {
		if (!codec_write(codec_la, WCD_REGISTERS + (reg & 0xff), want)) {
			codec_failed = true;
			return false;
		}
		codec_writes++;
	}
	if (!reg_read(reg, &back)) {
		return false;
	}
	if (((back ^ want) & mask) != 0) {
		codec_mismatches++;
		say("slim: codec register %#06x: %#04x written (mask %#04x), %#04x read back\n", reg, want, mask, back);
	}
	return true;
}

static bool
reg_put(uint16_t reg, uint8_t value)
{
	return reg_update(reg, 0xff, value);
}

static void
pause_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* wcd934x_bring_up() and the MCLK inside the codec (wcd934x_enable_ana_bias_and_sysclk()) */
static bool
codec_power_on(void)
{
	say("slim: the codec brought up (wcd934x_bring_up) and its MCLK enabled inside it\n");
	reg_put(R_CODEC_RPM_RST_CTL, 0x01);
	reg_put(R_SIDO_NEW_VOUT_A_STARTUP, 0x19);
	reg_put(R_SIDO_NEW_VOUT_D_STARTUP, 0x15);
	pause_us(1000);
	reg_put(R_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x5);
	reg_put(R_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x7);
	reg_put(R_CODEC_RPM_RST_CTL, 0x3);
	reg_put(R_CODEC_RPM_RST_CTL, 0x7);
	reg_put(R_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x3);
	reg_update(R_CODEC_RPM_CLK_MCLK_CFG, 0x03, 0x01);               /* 9.6 MHz (wcd934x_comp_set_sysclk) */
	reg_update(R_ANA_BIAS, 0x80, 0x80);
	reg_update(R_ANA_BIAS, 0x40, 0x40);                             /* pre-charge */
	pause_us(1000);
	reg_update(R_ANA_BIAS, 0x40, 0x00);
	reg_update(R_ANA_BIAS, 0x20, 0x00);
	reg_update(R_CLK_SYS_MCLK_PRG, 0x80, 0x80);                     /* the external clock's buffer */
	reg_update(R_CLK_SYS_MCLK_PRG, 0x30, 0x10);                     /* divided by 2 */
	reg_update(R_CLK_SYS_MCLK_PRG, 0x02, 0x00);                     /* the MCLK from the external clock */
	reg_update(R_CLK_SYS_MCLK_PRG, 0x01, 0x01);
	reg_update(R_CDC_CLK_RST_CTRL_FS_CNT_CONTROL, 0x01, 0x01);
	reg_update(R_CDC_CLK_RST_CTRL_MCLK_CONTROL, 0x01, 0x01);
	reg_update(R_CODEC_RPM_CLK_GATE, 0x03, 0x00);
	pause_us(15);
	reg_update(R_ANA_RCO, 0x80, 0x80);                              /* SIDO from the RCO and the band gap */
	pause_us(110);
	return !codec_failed;
}

static void
codec_power_off(void)
{
	codec_failed = false;
	reg_update(R_CLK_SYS_MCLK_PRG, 0x81, 0x00);
	reg_update(R_ANA_BIAS, 0x80, 0x00);
	reg_update(R_ANA_BIAS, 0x40, 0x00);
}

static bool ear_on;

/* The "handset" path powered as DAPM powers it, without samples (the head) */
static bool
ear_path_on(void)
{
	say("slim: the earpiece path powered (RX0 -> RX INT0 -> class H -> EAR PA), with no samples: silence\n");
	reg_update(R_ANA_RX_SUPPLIES, 0x01, 0x01);                      /* RX_BIAS */
	reg_update(R_CDC_RX0_RX_PATH_CTL, 0x20, 0x20);                  /* INT0_CLK */
	reg_update(R_CDC_RX0_RX_PATH_DSMDEM_CTL, 0x01, 0x01);           /* DSMDEM0_CLK */
	reg_update(R_CDC_RX_INP_MUX_RX_INT0_CFG0, 0x0f, 5);             /* RX INT0_1 MIX1 INP0: RX0 */
	reg_update(R_CDC_RX0_RX_PATH_CFG0, 0x08, 0x08);                 /* the DEM's look-ahead delay */
	reg_update(R_CDC_RX0_RX_PATH_SEC0, 0x01, 0x01);                 /* RX INT0 DEM MUX: CLSH_DSM_OUT */
	reg_update(R_CDC_RX0_RX_VOL_CTL, 0xff, (uint8_t)-20);           /* -20 dB, for a start */
	/* EAR DAC, PRE_PMU: the autochop timer off; class H for the EAR (CLS_H_NORMAL) */
	reg_update(R_HPH_NEW_INT_HPH_TIMER1, 0x02, 0x00);
	reg_update(R_CDC_CLSH_CRC, 0x01, 0x01);
	reg_update(R_CDC_RX0_RX_PATH_CFG0, 0x40, 0x40);
	reg_update(R_ANA_RX_SUPPLIES, 0x08, 0x00);                      /* buck: default level */
	reg_update(R_ANA_RX_SUPPLIES, 0x04, 0x00);                      /* flyback: default level */
	reg_update(R_ANA_RX_SUPPLIES, 0x40, 0x40);                      /* flyback on */
	pause_us(610);
	reg_update(R_RX_BIAS_FLYB_BUFF, 0x0f, 0x0a);
	reg_update(R_RX_BIAS_FLYB_BUFF, 0xf0, 0xa0);
	pause_us(110);
	reg_update(R_ANA_RX_SUPPLIES, 0x80, 0x80);                      /* buck on */
	pause_us(510);
	/* EAR PA */
	reg_update(R_ANA_EAR, 0x80, 0x80);
	ear_on = true;
	pause_us(5500);
	reg_update(R_CDC_RX0_RX_PATH_CTL, 0x10, 0x00);                  /* unmuted */
	reg_update(R_CDC_RX0_RX_PATH_MIX_CTL, 0x10, 0x00);
	return !codec_failed;
}

static void
ear_path_off(void)
{
	codec_failed = false;
	if (ear_on) {
		reg_update(R_CDC_RX0_RX_PATH_CTL, 0x10, 0x10);          /* muted */
		reg_update(R_ANA_EAR, 0x80, 0x00);
		pause_us(5500);
		ear_on = false;
	}
	reg_update(R_CDC_RX0_RX_PATH_CFG0, 0x40, 0x00);
	reg_update(R_CDC_CLSH_CRC, 0x01, 0x00);
	reg_update(R_ANA_RX_SUPPLIES, 0x80, 0x00);                      /* buck off */
	pause_us(510);
	reg_update(R_ANA_RX_SUPPLIES, 0x40, 0x00);                      /* flyback off */
	pause_us(610);
	reg_update(R_CDC_RX0_RX_PATH_SEC0, 0x01, 0x00);
	reg_update(R_CDC_RX0_RX_PATH_CFG0, 0x08, 0x00);
	reg_update(R_CDC_RX_INP_MUX_RX_INT0_CFG0, 0x0f, 0);
	reg_update(R_CDC_RX0_RX_PATH_DSMDEM_CTL, 0x01, 0x00);
	reg_update(R_CDC_RX0_RX_PATH_CTL, 0x20, 0x00);                  /* INT0's clock off, then its reset */
	reg_update(R_CDC_RX0_RX_PATH_CTL, 0x40, 0x40);
	reg_update(R_CDC_RX0_RX_PATH_CTL, 0x40, 0x00);
	reg_update(R_ANA_RX_SUPPLIES, 0x01, 0x00);
}

static void
ear_look(const char *when)
{
	uint8_t ear = 0, status = 0, supplies = 0, ctl = 0;

	codec_failed = false;
	reg_read(R_ANA_EAR, &ear);
	reg_read(R_EAR_STATUS_REG, &status);
	reg_read(R_ANA_RX_SUPPLIES, &supplies);
	reg_read(R_CDC_RX0_RX_PATH_CTL, &ctl);
	say("slim: %s: ANA_EAR %#04x, EAR_STATUS %#04x, RX supplies %#04x, RX0 path ctl %#04x\n", when, ear, status, supplies, ctl);
}

#define PIN_CFG_8MA_PULL_DOWN_OUT       (1u | 3u << 6 | 1u << 9)        /* pull-down, 8 mA, output; function 0 (gpio) */
#define PIN_CFG_2MA_OUT                 (0u | 0u << 6 | 1u << 9)        /* no pull, 2 mA, output */

static void
codec_up(void)
{
	uint8_t generic = 0xff, interface = 0xff, id[4] = { 0 };
	uint64_t began;

	if (!mclk(true)) {
		return;
	}
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, 0x100000);
	reset_cfg = rd(tlmm_south + 0x1000UL * PIN_CODEC_RESET);
	reset_io = rd(tlmm_south + 0x1000UL * PIN_CODEC_RESET + 4);
	say("slim: the codec's reset released as wcd9xxx_reset() does it: gpio143 low (2 mA) for 20 ms, then high (8 mA)\n");
	wr(tlmm_south + 0x1000UL * PIN_CODEC_RESET + 4, 0);
	wr(tlmm_south + 0x1000UL * PIN_CODEC_RESET, PIN_CFG_2MA_OUT);
	reset_released = true;
	delay_for_interval(20, NSEC_PER_MSEC);
	wr(tlmm_south + 0x1000UL * PIN_CODEC_RESET, PIN_CFG_8MA_PULL_DOWN_OUT);
	wr(tlmm_south + 0x1000UL * PIN_CODEC_RESET + 4, 2);
	began = now_us();
	while (now_us() - began < 2000000 && (generic == 0xff || interface == 0xff)) {
		if (interface == 0xff) {
			interface = address_query(ea_interface);
		}
		if (generic == 0xff) {
			generic = address_query(ea_generic);
		}
		if (generic == 0xff || interface == 0xff) {
			delay_for_interval(50, NSEC_PER_MSEC);
		}
	}
	say("slim: %llu ms after the reset: the codec's interface device %s%#x, its generic device %s%#x\n",
	    (now_us() - began) / 1000, interface == 0xff ? "has NO logical address " : "is ", interface,
	    generic == 0xff ? "has NO logical address " : "is ", generic);
	if (generic == 0xff) {
		return;
	}
	if (codec_write(generic, WCD_REGISTERS + 0x00, 0) && codec_read(generic, WCD_REGISTERS + 0x21, id, 4)) {
		uint16_t minor = (uint16_t)(id[0] | id[1] << 8), major = (uint16_t)(id[2] | id[3] << 8);

		say("slim: THE CODEC'S CHIP ID: %02x %02x %02x %02x: major %#x%s, minor %#x%s\n", id[0], id[1], id[2], id[3], major,
		    major == TAVIL_MAJOR ? " (Tavil, a WCD9340/9341)" : " (NOT Tavil's 0x108)", minor,
		    minor == 0 ? " (1.0)" : minor == 1 ? " (1.1)" : "");
		if (level >= 4 && major == TAVIL_MAJOR) {
			codec_la = generic;
			codec_page = 0;
			if (codec_power_on()) {
				ear_look("before the path");
				if (ear_path_on()) {
					ear_look("the path on");
					delay_for_interval(2000, NSEC_PER_MSEC);
					ear_look("two seconds later");
				}
				ear_path_off();
				ear_look("the path off");
			}
			codec_power_off();
			say("slim: the codec: %u registers changed, %u did not read back as written\n", codec_writes, codec_mismatches);
		}
	}
}

static void
codec_down(void)
{
	if (reset_released) {
		wr(tlmm_south + 0x1000UL * PIN_CODEC_RESET + 4, reset_io & 2);
		wr(tlmm_south + 0x1000UL * PIN_CODEC_RESET, reset_cfg);
		reset_released = false;
		say("slim: the codec back into reset (gpio143 as it was found)\n");
	}
	if (mclk_on) {
		(void)mclk(false);
	}
}

static void
ngd_down(void)
{
	if (!ngd_enabled) {
		return;
	}
	wr(ngd + NGD_CFG, 0);
	wr(ngd + NGD_INT_EN, 0);
	ngd_enabled = false;
	say("slim: the NGD disabled again (cfg %#x)\n", rd(ngd + NGD_CFG));
}

static void
look(void)
{
	vm_offset_t slim = slim_regs;
	vm_offset_t tlmm = ml_io_map(TLMM_SOUTH_PHYS, 0x100000);
	uint32_t version = rd(slim), cfg = rd(tlmm + 0x1000UL * PIN_CODEC_RESET), io = rd(tlmm + 0x1000UL * PIN_CODEC_RESET + 4);

	say("slim: the controller's version word %#x (version %#x)\n", version, version >> 16);
	say("slim: the NGD (%#lx): cfg %#x, status %#x%s, rx msgq cfg %#x, int en %#x, int stat %#x\n", SLIM_PHYS + NGD_OFFSET,
	    rd(ngd + NGD_CFG), rd(ngd + NGD_STATUS), (rd(ngd + NGD_STATUS) & 2) ? " (a logical address is ours)" : "",
	    rd(ngd + NGD_RX_MSGQ_CFG), rd(ngd + NGD_INT_EN), rd(ngd + NGD_INT_STAT));
	say("slim: the codec's reset, gpio%u: function %u, pull %u, %u mA, %s, reads %u\n", PIN_CODEC_RESET, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & (1u << 9)) ? ((io & 2) ? "drives high: the codec out of reset" :
	    "drives low: the codec in reset") : "input", io & 1);
}

static void
slim_thread(__unused void *parameter, __unused wait_result_t result)
{
	const uint8_t types[2] = { 0x01, 0x10 };
	const uint32_t values[2] = { SLIM_QMI_INSTANCE, SLIM_MODE_MASTER };
	uint64_t began = now_us();

	say("slim: level %u: the DSP asked for the SLIMbus, the NGD looked at%s, the power given back\n", level,
	    level >= 2 ? ", enabled and the capabilities exchanged" : "");
	while (!hakios_adsp_qrtr_find(SLIM_QMI_SERVICE, SLIM_QMI_VERSION, SLIM_QMI_INSTANCE, &dsp_node, &dsp_port)) {
		if (now_us() - began > 5000000) {
			say("slim: the DSP has not told of its SLIMbus service (0x301) within 5 s: nothing is done\n");
			goto done;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	our_port = hakios_adsp_qrtr_open(qmi_receive);
	if (our_port == 0) {
		say("slim: no QRTR port of ours left\n");
		goto done;
	}
	say("slim: the DSP's SLIMbus service is at node %u port %u; ours is port %#x\n", dsp_node, dsp_port, our_port);
	if (!qmi_request("select instance 0, the DSP the master", QMI_SELECT_INSTANCE, types, values, 2) || !bus_power(true)) {
		goto done;
	}
	slim_regs = ml_io_map(SLIM_PHYS, SLIM_SIZE);
	ngd = slim_regs + NGD_OFFSET;
	look();
	if (level >= 2) {
		ngd_up();
		if (level >= 3 && (rd(ngd + NGD_STATUS) & 2)) {
			codec_up();
			codec_down();
		}
		ngd_down();
	}
	(void)bus_power(false);
done:
	if (our_port != 0) {
		hakios_adsp_qrtr_close(our_port);
		our_port = 0;
	}
	say("slim: done\n");
	thread_terminate(current_thread());
}

void
hakios_slim_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("slim", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 4 ? 4 : level;
	if (kernel_thread_start(slim_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
hakios_slim_dsp_down(void)
{
	/* (the power is given back at the level's end; a run that ends before has the DSP shut down, which drops it too) */
}
