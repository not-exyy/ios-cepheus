/*
 * The Wi-Fi of the Xiaomi Mi 9 (cepheus, SM8150), from nothing and step by step: stage 0, a survey that reads and
 * writes nothing but the PMIC arbiter's command word (steps 1 to 3); stage 2, the firmware's QMI service (4 to 7);
 * stage 3 so far, the radio's view of memory, its supplies and mission mode (8 to 10).
 *
 * The radio is a WCN3998 (of the WCN3990 family; dt qcom,icnss@18800000, Linux's ath10k_snoc). It is not the SM7325
 * phone's WCN6750 (hakios.wifi.c): its firmware does not run on a DSP of its own but in a "protection domain" of the
 * modem, the WLAN PD (modemuw.jsn: msm/modem/wlan_pd), which the modem's root PD loads from wlanmdsp.mbn that the
 * application processor serves it over TFTP. So the path is: the modem started (drivers/modem/hakios.mpss.c, with a
 * board for SM8150), the WLAN PD up, the firmware's QMI service WLFW (0x45) on the modem's QRTR node, the radio's
 * four supplies on, the MSA memory given to the modem, the board's data file sent; then the copy engines at
 * 0x18800000, whose rings the radio reaches through the application processors' SMMU as stream 0x640. The whole
 * plan, with what is reused from the SM7325 phone's Wi-Fi: HANDOFF_RU.md, "Wi-Fi на cepheus".
 *
 * Boot-arg wcn_step=N performs steps 1 to N and stops. Every one only reads:
 *
 *   1  reads   the shared memory (smem@86000000, 2 MB): its table of partitions, the partition of us and the modem
 *              (hosts 0 and 1), its items; whether the modem's SMP2P item (435), ours (428), its last words (421) and
 *              GLINK's three (478, 479, 480) are there -- they are if the modem has run since the phone was started
 *   2  reads   the radio's four supplies through the PMIC arbiter's observer, as the boot loader leaves them:
 *              vdd-cx-mx  PM8150 L1  (0.752 V)    vdd-1.8-xo  PM8150 L7   (1.8 V)
 *              vdd-1.3-rfa PM8150L L2 (1.304 V)   vdd-3.3-ch0 PM8150L L11 (3.1-3.3 V)
 *              (dt: the supplies of qcom,icnss@18800000 and their rpmh-regulator-ldoa1/a7/c2/c11). The only thing
 *              written is the command word that asks the arbiter for a read, as hakios.pmhaptic.c does
 *   3  reads   the application processors' SMMU (0x15000000): which stream match registers there are, which cover
 *              the radio's stream 0x640 (dt: iommus <&apps_smmu 0x640 0x1>), and what they do with it
 *
 * Steps 4 to 7 run on a thread of their own once the modem's QRTR is there (hakios.mpss.c, modem_step=10 modem_keep=1;
 * the modem stays offline), and speak ath10k's WLFW messages (below, "stage 2"):
 *
 *   4  talks   waits for the modem's QRTR (wcn_wait=S, default 180) and WLFW (0x45) on it; which indications we want
 *   5  talks   what the host can do; where the firmware's memory is (0x8bc00000, 1 MB): the regions it wants of it
 *   6  asks    TrustZone to hand those regions from HLOS to MSS_MSA and WLAN (and WLAN_CE where not secure); "MSA
 *              ready"; what the chip and the firmware are (chip, board and soc ids, the firmware's version)
 *   7  talks   the board's data (fw/image/bdwlan.bXX for that board id) in pieces of 6 KB, the calibration report;
 *              waits for "FW READY" (wcn_ready=S, default 15)
 *
 * Nothing of stage 2 touches the radio's supplies, its registers or the SMMU: "FW READY" comes without them (ath10k
 * powers the radio only after it, ath10k_snoc_hif_power_up()). Steps 8 to 10 do what ath10k does next, by hand, so
 * that each can be seen to work before ath10k itself is brought in (tools/mk-ath10k.sh, wifi/lx: still to come):
 *
 *   8  writes  the SMMU: the radio's stream 0x640 gets a free match register and context with page tables of ours,
 *              and is shown 64 KB of ours at 0xa0000000 (hakios_wcn3990_radio_map(), for ath10k's rings later)
 *   9  votes   with wcn_power=1 only: the radio's four supplies on through RPMh, each at the voltage and mode it is
 *              already set to, read back one by one; then its reference clock (RF_CLK2). Without it, nothing
 *  10  talks   the copy engines' configuration (snoc.c's tables), mission mode; watches the firmware for wcn_watch=S
 *              (default 10) seconds; wcn_ce_peek=1 reads the copy engines' registers after that
 *
 * When the steps are over the mode goes back to "off", the supplies and the clock off, the SMMU as it was.
 *
 * More boot-args: wcn_pause=MS, how long a step waits after saying what it is about to read (default 2200: the
 * loader, watching from another core, notes the end of our log every two seconds, and after a silent reset that is
 * all there is); wcn_return=1, go back to the loader when the steps are over instead of starting the system.
 *
 * Nothing is done on another phone than cepheus (/arm-io's device_type, as the loader writes it). The radio's own
 * registers (0x18800000) are not read: on the SM7325 phone a read there before the firmware was in mission mode
 * ended the kernel.
 *
 * Checked: compiled, and on the phone (2026-11-06, wcn_step=3 wcn_return=1, the system not started):
 *   1  (logs/211743-run.log) the partition of us and the modem is at 0x860e3000, 0x7c000 bytes, empty: no item 435, 428,
 *      421, 478..480 -- the modem has not run since the phone was started, and its items are ours to make (as the audio
 *      DSP's were, hakios.adsp.c)
 *   2  (logs/212927-run.log) the regulators are on each PMIC's second slave (the arbiter's list, logs/212754-run.log); all
 *      four LDOs are type 0x04 (LDO), written by system 4 (the always-on processor's: RPMh), voltage already set to
 *      Android's (752, 1800, 1304, 3104 mV), mode 0x07, status 0x03, enable 0: OFF. Turning the radio's supplies on is a
 *      vote to RPMh for "on" at the voltage each already has
 *   3  (logs/212754-run.log) scr0 0x2f0406: on, a stream nobody matches faults; 94 match registers of which 5 are in use,
 *      56 contexts; none covers 0x640. The first free match register is 5, the lowest unused context 4: the radio's
 *      stream needs a match, a context and page tables of ours, as on the SM7325 phone (hakios.wifi.c, wifi_smmu=2)
 * And stage 2 (2026-11-06, modem_step=10 modem_new=11 modem_keep=1 wcn_step=7 wcn_return=1; logs/220110, 220547,
 * 220858-run.log):
 *   4  the modem's QRTR is up when the step begins; WLFW at node 0 port 81 within 2 ms; the indications taken, status 0
 *   5  host_cap taken; the firmware wants two regions of the MSA: 0x8bc00000, 16 KB, secure; 0x8bc04000, 1008 KB, not
 *   6  TrustZone hands both over; "MSA ready" indicated; chip id 0x30224, family 0x4001, soc id 0x40060000, no board id
 *      (0xff: bdwlan.bin), firmware 0x30098158 "WLAN.HL.3.0.c2-00344-QCAHLSWMTPLZ-1", built 2021-05-19
 *   7  bdwlan.bin (26328 bytes) taken in 5 pieces; the calibration report taken; without the qdss clock the WLAN PD
 *      died 13 ms later (see step_board()), with it "FW READY" comes 3.4 s after the report. The modem stops when asked
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.sta.h>
#include <sys/hakios.qrtr.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

#define STEPS                   10

static uint32_t step, pause_ms = 2200;

/* ---- saying ------------------------------------------------------------------------------------------- */

static int say(const char *format, ...) __printflike(1, 2);

/* To the loader's log and the kernel's. */
static int
say(const char *format, ...)
{
	char line[400];
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

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
announce(const char *what)
{
	say("wcn: step %u: %s\n", step, what);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

static bool
is_cepheus(void)
{
	DTEntry entry;
	void const *prop;
	unsigned int size;

	return SecureDTLookupEntry(NULL, "/arm-io", &entry) == kSuccess &&
	       SecureDTGetProperty(entry, "device_type", &prop, &size) == kSuccess && size >= 7 &&
	       strncmp((const char *)prop, "cepheus", size) == 0;
}

/* ---- step 1: the shared memory (layouts as hakios.adsp.c has them, from Linux's qcom/smem.c) ---------------- */

#define SMEM_PHYS               0x86000000UL    /* dt: reserved-memory smem */
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_MODEM              1               /* dt: qcom,remote-pid of qcom,smp2p-modem and qcom,glink/modem */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5
#define SMEM_SOCINFO            137             /* the boot firmware's socinfo (Linux: SMEM_HW_SW_BUILD_ID) */
#define SOCINFO_SERIAL          96              /* ... its serial_num */
#define ITEM_SMP2P_THEIRS       435             /* dt: qcom,smem <435 428> of qcom,smp2p-modem@1799000c */
#define ITEM_SMP2P_OURS         428
#define ITEM_CRASH_REASON       421             /* dt: qcom,smem-id of qcom,mss@4080000 */

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};
struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (smem == NULL || offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size ||
		    entry.padding_data > entry.size ||
		    (uint64_t)at + sizeof(entry) + entry.padding_header + entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at ||
		    entry.padding_data > entry.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

static struct smem_item found[256];

/* Where an item of ours and the modem's is, in the partition of the two or else in the global one; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_MODEM }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_smp2p(uint32_t item, const char *whose)
{
	struct smp2p words;
	uint32_t size = 0, at = smem_item(item, &size);
	char line[320];
	int n = 0;

	if (at == 0) {
		say("wcn: 1: item %u (%s SMP2P): not there\n", item, whose);
		return;
	}
	if (size < sizeof(words) || !smem_peek(at, &words, sizeof(words)) || words.magic != SMP2P_MAGIC) {
		say("wcn: 1: item %u (%s SMP2P): THERE, %u bytes, but not SMP2P's\n", item, whose, size);
		return;
	}
	line[0] = 0;
	for (unsigned i = 0; i < words.valid && i < 16 && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %.16s = %#x;", words.entry[i].name, words.entry[i].value);
	}
	say("wcn: 1: item %u (%s SMP2P): THERE: version %u, from %u to %u, flags %#x, %u of %u entries:%s\n", item, whose,
	    words.version & 0xff, words.local, words.remote, words.flags, words.valid, words.total, line);
}

static bool
step_smem(void)
{
	static const uint32_t glink[] = { 478, 479, 480 };
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition, crash_size = 0, crash_at, item_size = 0;
	unsigned count;
	char line[320];

	announce("read the shared memory (0x86000000, 2 MB): the table of partitions, the partition of us and the modem, its "
	    "items");
	partition = smem_partition(SMEM_US, SMEM_MODEM, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		say("wcn: 1: there is no partition for us and the modem (hosts 0 and 1), or the shared memory has no table\n");
		return false;
	}
	count = smem_items(partition, cacheline, found, 256);
	say("wcn: 1: the partition for us and the modem: at %#lx, %#x bytes, of which %#x free, cache line %u; %u items\n",
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, cacheline, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		say("wcn: 1:   items (bytes):%s\n", line);
	}
	show_smp2p(ITEM_SMP2P_THEIRS, "the modem's");
	show_smp2p(ITEM_SMP2P_OURS, "our");
	for (unsigned g = 0; g < 3; g++) {
		uint32_t at = smem_item(glink[g], &item_size);

		say("wcn: 1: item %u (GLINK's): %s", glink[g], at ? "THERE" : "not there\n");
		if (at) {
			say(", %u bytes\n", item_size);
		}
	}
	crash_at = smem_item(ITEM_CRASH_REASON, &crash_size);
	if (crash_at == 0 || crash_size == 0) {
		say("wcn: 1: item %u, the modem's last words: not there\n", ITEM_CRASH_REASON);
	} else {
		char text[81];

		bzero(text, sizeof(text));
		smem_peek(crash_at, text, crash_size < sizeof(text) - 1 ? crash_size : sizeof(text) - 1);
		say("wcn: 1: item %u, the modem's last words (%u bytes): \"%s\"\n", ITEM_CRASH_REASON, crash_size, text);
	}
	say("wcn: 1: the modem has %s since the phone was started\n", smem_item(ITEM_SMP2P_THEIRS, &item_size) ? "RUN" :
	    "not run");
	return true;
}

/* ---- step 2: the radio's supplies, through the PMIC arbiter (version 5, as hakios.pmhaptic.c has it) ---------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000

static vm_offset_t arb_core, arb_cnfg, arb_observer;

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

/* Read `count' (1 to 8) registers from `reg' on of the peripheral behind `channel'. NULL, or what went wrong. */
/*
 * Up to READ_TRIES times: the bus drops a transfer now and then, with every other master of it busy (logs/193127, a run
 * of the kit with the fuel gauge, the keys, the vibration and the temperatures all reading the PMICs: three of the
 * first reads of the supplies dropped, and the radio was taken for one that did not come up). The arbiter refusing
 * is not tried again.
 */
#define READ_TRIES              5

static const char *
spmi_read(uint32_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t c = arb_observer + 0x80UL * channel;
	uint64_t start, interval;
	uint32_t status, data[2];
	const char *problem;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	for (unsigned tries = 1;; tries++) {
		start = mach_absolute_time();
		hakios_spmi_lock();
		wr(c + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
		do {
			status = rd(c + OBSERVER_STATUS);
		} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
		data[0] = rd(c + OBSERVER_DATA0);
		data[1] = count > 4 ? rd(c + OBSERVER_DATA1) : 0;
		hakios_spmi_unlock();
		problem = status_problem(status);
		if (problem == NULL || (status & STATUS_DENIED) || tries == READ_TRIES) {
			break;
		}
		delay(100);
	}
	if (problem == NULL) {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	return problem;
}

/*
 * The four LDOs: slave and peripheral, and what Android asks of each (dt: qcom,vdd-*-config of qcom,icnss@18800000
 * and the rpmh-regulator's limits), in mV. A PMIC5 has its regulators on its second slave: the PM8150's on 1, the
 * PM8150L's on 5 (an LDO n there is peripheral 0x40 + n - 1). The first try (logs/211743-run.log) looked on slaves 0
 * and 4 and found no channel; the arbiter's list (logs/212754-run.log) has peripherals 0x40..0x51 on slave 1 and
 * 0x40..0x4a on slave 5.
 */
static const struct {
	const char *supply, *ldo;
	uint16_t peripheral;
	uint16_t min_mv, max_mv;
} supplies[] = {
	{ "vdd-cx-mx", "PM8150 L1", 0x140, 752, 752 },
	{ "vdd-1.8-xo", "PM8150 L7", 0x146, 1800, 1800 },
	{ "vdd-1.3-rfa", "PM8150L L2", 0x541, 1304, 1304 },
	{ "vdd-3.3-ch0", "PM8150L L11", 0x54a, 3104, 3304 },
};
#define SUPPLIES                (sizeof(supplies) / sizeof(supplies[0]))
static uint32_t supply_channel[SUPPLIES] = { ~0u, ~0u, ~0u, ~0u };     /* the arbiter's channel of each, as step 2 found it */

/*
 * A PMIC5 regulator's registers (Linux: qcom_spmi-regulator.c and Qualcomm's documented layout): 0x04/0x05 the
 * peripheral's type and subtype, 0x08 status (bit 7: the output is up), 0x40/0x41 the voltage set, in mV, 0x45
 * the mode, 0x46 enable (bit 7). Read as they are; what they mean here is a reading to check against Android's.
 */
static bool
step_supplies(void)
{
	uint32_t version, channels;
	unsigned seen = 0;

	announce("read the radio's four supplies through the PMIC arbiter's observer (the only write: the arbiter's command "
	    "word for a read)");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS + 0x10000UL * ARB_SYSTEM, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	say("wcn: 2: the PMIC arbiter is version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("wcn: 2: not the arbiter this driver knows (version 5): nothing is read\n");
		return false;
	}
	if (channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	/*
	 * Which peripherals the arbiter has channels for, by slave: on the phone (2026-11-06, logs/211743-run.log) none of the
	 * four LDOs had one, and the regulators may be the always-on processor's alone. The list says what there is.
	 */
	for (uint32_t sid = 0; sid < 16; sid++) {
		char line[360];
		int at = 0;
		unsigned count = 0;

		for (uint32_t n = 0; n < channels; n++) {
			uint32_t map = rd(arb_core + ARB_CHANNEL(n));

			if (map == 0 || ((map >> 16) & 0xf) != sid) {
				continue;
			}
			count++;
			if (at < (int)sizeof(line) - 8) {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", (map >> 8) & 0xff);
			}
		}
		if (count != 0) {
			say("wcn: 2: slave %u: %u channels, peripherals:%s\n", sid, count, line);
		}
	}
	for (unsigned s = 0; s < SUPPLIES; s++) {
		uint32_t channel = ~0u, owner = ~0u;
		uint8_t type[2] = { 0 }, status = 0, voltage[2] = { 0 }, mode_enable[2] = { 0 };
		const char *problem;

		for (uint32_t n = 0; n < channels; n++) {
			uint32_t map = rd(arb_core + ARB_CHANNEL(n));

			if (map != 0 && ((map >> 8) & 0xfff) == supplies[s].peripheral) {
				channel = n;
				supply_channel[s] = n;
				owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
				break;
			}
		}
		if (channel == ~0u) {
			say("wcn: 2: %s (%s, %#x): no channel of the arbiter is it\n", supplies[s].supply, supplies[s].ldo,
			    supplies[s].peripheral);
			continue;
		}
		if ((problem = spmi_read(channel, 0x04, type, 2)) != NULL || (problem = spmi_read(channel, 0x08, &status, 1)) != NULL ||
		    (problem = spmi_read(channel, 0x40, voltage, 2)) != NULL ||
		    (problem = spmi_read(channel, 0x45, mode_enable, 2)) != NULL) {
			say("wcn: 2: %s (%s, channel %u, written by system %u): %s\n", supplies[s].supply, supplies[s].ldo, channel,
			    owner, problem);
			continue;
		}
		seen++;
		say("wcn: 2: %s (%s, channel %u, written by system %u): type %#x/%#x, status %#x, set %u mV, mode %#x, enable %#x: "
		    "%s, %s (Android: %u..%u mV)\n", supplies[s].supply, supplies[s].ldo, channel, owner, type[0], type[1], status,
		    (unsigned)(voltage[1] << 8 | voltage[0]), mode_enable[0], mode_enable[1], mode_enable[1] & 0x80 ? "ON" : "off",
		    status & 0x80 ? "up" : "not up", supplies[s].min_mv, supplies[s].max_mv);
	}
	if (seen == 0) {
		say("wcn: 2: none of the four supplies could be read from here: they are left to RPMh, and the steps go on\n");
	}
	return true;
}

/* ---- step 3: the radio's stream in the SMMU (as hakios.wifi.c's smmu_look() and hakios.sound.c have it) -------- */

#define SMMU_PHYS               0x15000000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define RADIO_STREAM            0x640           /* dt: iommus <&apps_smmu 0x640 0x1> of qcom,icnss@18800000 */
#define RADIO_STREAM_MASK       0x1

static bool
step_smmu(void)
{
	static const char *const kinds[] = { "translated by context", "PASSED UNTRANSLATED (bypass; the number is",
		                             "MADE TO FAULT (the number is", "of a reserved kind (the number is" };
	bool used[256] = { false };
	vm_offset_t smmu;
	uint32_t scr0, idr0, idr1, matches, contexts, valid = 0, covering = 0, free_match = ~0u, free_context;

	announce("read the global registers of the application processors' SMMU (0x15000000) and its stream match registers");
	smmu = ml_io_map(SMMU_PHYS, 0x2000);
	scr0 = rd(smmu + SMMU_SCR0);
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	say("wcn: 3: scr0 %#x, idr0 %#x, idr1 %#x: the SMMU is %s; %u stream match registers, %u contexts; a stream nobody "
	    "matches %s%s\n", scr0, idr0, idr1, scr0 & 1 ? "OFF" : "on", matches, contexts, scr0 & 1u << 10 ? "faults" :
	    "passes untranslated", scr0 & 1u << 3 ? "; streams of 16 bits" : "");
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t wide = scr0 & 1u << 3, stream = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

		if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
			if (free_match == ~0u) {
				free_match = n;
			}
			continue;
		}
		valid++;
		if ((s2cr >> 16 & 3) == 0) {
			used[s2cr & 0xff] = true;
		}
		if (((stream ^ RADIO_STREAM) & ~(mask | RADIO_STREAM_MASK)) != 0) {
			continue;
		}
		say("wcn: 3: match register %u covers the radio's stream: stream %#x mask %#x (%#x), %s %u; s2cr %#x)\n", n, stream,
		    mask, smr, kinds[(s2cr >> 16) & 3], s2cr & 0xff, s2cr);
		covering++;
	}
	for (free_context = 0; free_context < contexts && free_context < 256 && used[free_context]; free_context++) {
	}
	say("wcn: 3: %u of the %u match registers are in use; %u cover stream %#x%s; the first free match register %d, the "
	    "lowest context no valid match register uses %u\n", valid, matches, covering, RADIO_STREAM, covering ? "" :
	    ": THE BOOT CHAIN HAS SET NOTHING UP FOR THE RADIO'S STREAM", free_match == ~0u ? -1 : (int)free_match, free_context);
	return true;
}

/* ---- stage 3, step 8: the radio's view of memory (as hakios.wifi.c's wifi_smmu=2 makes it for the WCN6750) ------- */

/*
 * What the radio reads and writes by itself (its copy engines' rings, the data path's buffers) goes through the
 * application processors' SMMU as stream 0x640, and step 3 found that nothing covers it and that a stream nobody matches
 * faults. So the stream gets the first free match register and the lowest context nobody uses, with page tables of
 * ours, as on the SM7325 phone (hakios.wifi.c, where the formats and the order come from; the hypervisor grants these
 * registers). The radio's addresses are where Android's tree has them (dt: qcom,iommu-dma-addr-pool of qcom,icnss:
 * 256 MB from 0xa0000000), handed out upwards; its faults are recorded and do not stop it, and nobody takes their
 * interrupt (space_faults() looks). Everything written is put back when the steps are over.
 *
 * Step 8 shows the radio 64 KB of ours at 0xa0000000 -- which it is not yet told of: the point is that the SMMU takes
 * what is written. ath10k's rings come later through hakios_wcn3990_radio_map().
 */
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054
#define SMMU_CBAR(n)            (4 * (n))       /* in the second page */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define RADIO_ADDRESSES         0xa0000000ull
#define RADIO_ADDRESSES_SIZE    0x10000000u
#define RADIO_TABLES            32
#define RADIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define RADIO_TEST_SIZE         0x10000u

extern lck_mtx_t hakios_smmu_lock, hakios_rsc_lock;
extern uint64_t kvtophys(vm_offset_t va);

static struct {
	bool found, on;
	uint32_t page, pages;           /* of the SMMU's registers: a page's size, and how many before the contexts' */
	uint32_t match, context;
	uint32_t old_smr, old_s2cr, old_sctlr;
	vm_offset_t global, second, bank, tables;
	uint32_t table_count;
	uint64_t next;                  /* of the radio's addresses, the first not handed out */
} space;

/* A register of the SMMU written, and what it reads back said. */
static void
sput(vm_offset_t base, const char *name, uint32_t offset, uint32_t value)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy" ::: "memory");
	say("wcn: %u:   %s (+%#x) <- %#x, reads %#x\n", step, name, offset, value, rd(base + offset));
}

static uint64_t *
space_table(uint32_t n)
{
	return (uint64_t *)(space.tables + 4096 * n);
}

static uint64_t *
space_next(uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space.table_count; i++) {
			if (kvtophys((vm_offset_t)space_table(i)) == (*entry & PTE_ADDRESS)) {
				return space_table(i);
			}
		}
		return NULL;
	}
	if (space.table_count == RADIO_TABLES) {
		return NULL;
	}
	table = space_table(space.table_count++);
	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

static bool
space_enter(uint64_t address, uint64_t physical, uint64_t size, uint64_t attributes)
{
	if (space.tables == 0) {
		if (kmem_alloc(kernel_map, &space.tables, round_page(4096 * RADIO_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		space.table_count = 1;
	}
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = address + offset, *level2 = space_next(&space_table(0)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : space_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			say("wcn: %u: no page table left for the radio's %#llx\n", step, at);
			return false;
		}
		level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
	}
	for (vm_offset_t line = space.tables; line < space.tables + 4096 * RADIO_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

/* Which match register and which context are free; nothing is written. */
static bool
space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts;

	if (space.global == 0) {
		space.global = ml_io_map(SMMU_PHYS, 0x2000);
	}
	scr0 = rd(space.global + SMMU_SCR0);
	idr0 = rd(space.global + SMMU_IDR0);
	idr1 = rd(space.global + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	if (scr0 & 1u << 3) {
		say("wcn: %u: this SMMU numbers its streams with 16 bits, which nothing here is written for\n", step);
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * space.pages * space.page != 0x100000) {        /* dt: "reg" of apps-smmu */
		say("wcn: %u: the SMMU says it has 2 x %u pages of %#x bytes, not the megabyte of the device tree\n", step, space.pages,
		    space.page);
		return false;
	}
	space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(space.global + SMMU_SMR(n)), s2cr = rd(space.global + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		}
	}
	for (space.context = 0; space.context < contexts && used[space.context]; space.context++) {
	}
	if (space.match == ~0u || space.context >= contexts) {
		say("wcn: %u: the SMMU has no match register or no context left\n", step);
		return false;
	}
	space.second = ml_io_map(SMMU_PHYS + space.page, space.page);
	space.bank = ml_io_map(SMMU_PHYS + (uint64_t)(space.pages + space.context) * space.page, space.page);
	space.old_sctlr = rd(space.bank + SMMU_CB_SCTLR);
	say("wcn: %u: free: match register %u (smr %#x, s2cr %#x) and context %u at %#llx (sctlr %#x, cbar %#x, fsr %#x); "
	    "pages of %#x bytes, %u before the contexts'\n", step, space.match, space.old_smr, space.old_s2cr, space.context,
	    (unsigned long long)(SMMU_PHYS + (uint64_t)(space.pages + space.context) * space.page), space.old_sctlr,
	    rd(space.second + SMMU_CBAR(space.context)), rd(space.bank + SMMU_CB_FSR), space.page, space.pages);
	if (space.old_sctlr & 1) {
		say("wcn: %u: CONTEXT %u IS SWITCHED ON although no stream goes through it: it is somebody's, and is left alone\n",
		    step, space.context);
		return false;
	}
	space.found = true;
	return true;
}

/* What the SMMU has recorded of requests of the radio's that it could not serve. */
static void
space_faults(void)
{
	uint32_t status, global;

	if (!space.on) {
		return;
	}
	status = rd(space.bank + SMMU_CB_FSR);
	global = rd(space.global + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("wcn: %u: the SMMU has refused the radio nothing (context %u: fsr %#x; sgfsr 0)\n", step, space.context, status);
		return;
	}
	say("wcn: %u: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: "
	    "status %#x, syndromes %#x %#x (the second names the stream)\n", step, space.context, status,
	    (uint64_t)rd(space.bank + SMMU_CB_FAR + 4) << 32 | rd(space.bank + SMMU_CB_FAR), rd(space.bank + SMMU_CB_FSYNR0),
	    global, rd(space.global + SMMU_SGFSYNR0), rd(space.global + SMMU_SGFSYNR1));
}

/* The radio's stream through the context with our tables. */
static bool
space_switch_on(void)
{
	uint32_t smr = SMMU_SMR(space.match), s2cr = SMMU_S2CR(space.match);
	uint32_t sees = 1u << 31 | RADIO_STREAM_MASK << 16 | RADIO_STREAM;
	uint64_t root = kvtophys(space.tables) | (uint64_t)space.context << 48;

	say("wcn: %u: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets our page tables (root %#llx) and is switched on, and match "
	    "register %u sends stream %#x mask %#x through it\n", step, space.context, root & PTE_ADDRESS, space.match,
	    RADIO_STREAM, RADIO_STREAM_MASK);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
	sput(space.bank, "sctlr", SMMU_CB_SCTLR, 0);
	sput(space.second, "cba2r", SMMU_CBA2R(space.context), SMMU_CBA2R_64_BIT);
	sput(space.second, "cbar", SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	sput(space.bank, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	sput(space.bank, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	sput(space.bank, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)root);
	sput(space.bank, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	sput(space.bank, "ttbr1, low", SMMU_CB_TTBR1, 0);
	sput(space.bank, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	sput(space.bank, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	sput(space.bank, "mair1", SMMU_CB_MAIR1, 0);
	sput(space.bank, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	sput(space.bank, "tlbiasid", SMMU_CB_TLBIASID, space.context);
	sput(space.bank, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(space.bank + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("wcn: %u: the SMMU does not finish forgetting old translations\n", step);
			return false;
		}
		sleep_ms(1);
	}
	sput(space.bank, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;                        /* (from here on there is something to put back) */
	sput(space.global, "s2cr", s2cr, space.context);
	sput(space.global, "smr", smr, sees);
	if (rd(space.global + smr) != sees || (rd(space.global + s2cr) & 0x300ff) != space.context ||
	    (rd(space.second + SMMU_CBAR(space.context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(rd(space.bank + SMMU_CB_SCTLR) & 1) || rd(space.bank + SMMU_CB_TTBR0) != (uint32_t)root ||
	    rd(space.bank + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		say("wcn: %u: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x does not go through context %u\n", step, RADIO_STREAM,
		    space.context);
		return false;
	}
	say("wcn: %u: THE RADIO'S STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", step, space.context);
	space_faults();
	return true;
}

/* The stream and the context as they were found. */
static void
space_off(void)
{
	if (!space.on) {
		return;
	}
	space_faults();
	say("wcn: the radio's stream and context %u of the SMMU are put back as they were found\n", space.context);
	lck_mtx_lock(&hakios_smmu_lock);
	sput(space.global, "smr", SMMU_SMR(space.match), space.old_smr);
	sput(space.global, "s2cr", SMMU_S2CR(space.match), space.old_s2cr);
	sput(space.bank, "sctlr", SMMU_CB_SCTLR, space.old_sctlr);
	space.on = false;
	space.found = false;
	lck_mtx_unlock(&hakios_smmu_lock);
}

/*
 * Memory of ours (physical, size) that the radio is to see: the radio's address of it in *address. The first call
 * takes the match register and the context and switches them on.
 */
bool
hakios_wcn3990_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	bool ok;

	size = (size + 4095) & ~4095ull;
	if (space.next + size > RADIO_ADDRESSES_SIZE) {
		return false;
	}
	*address = RADIO_ADDRESSES + space.next;
	lck_mtx_lock(&hakios_smmu_lock);       /* (the camera and the sound may be looking for a free context too) */
	ok = (space.found || space_find()) && space_enter(*address, physical, size, RADIO_PTE);
	if (ok && !space.on) {
		ok = space_switch_on();
	} else if (ok) {
		wr(space.bank + SMMU_CB_TLBIASID, space.context);
		wr(space.bank + SMMU_CB_TLBSYNC, 1);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	if (ok) {
		space.next += size;
	}
	return ok;
}

static bool
step_space(void)
{
	vm_offset_t memory = 0;
	unsigned long long address = 0;

	announce("give the radio's stream (0x640) a free match register and context of the SMMU, with page tables of ours, "
	    "and show it 64 KB of ours at 0xa0000000");
	if (kmem_alloc_contig(kernel_map, &memory, RADIO_TEST_SIZE, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("wcn: 8: no 64 KB of memory in one piece\n");
		return false;
	}
	if (!hakios_wcn3990_radio_map(kvtophys(memory), RADIO_TEST_SIZE, &address)) {
		say("wcn: 8: THE RADIO'S STREAM COULD NOT BE GIVEN A CONTEXT\n");
		return false;
	}
	say("wcn: 8: the radio sees 64 KB of ours (%#llx) at %#llx; %u page tables in use\n", kvtophys(memory), address,
	    space.table_count);
	return true;
}

/* ---- stage 3, step 9: the radio's supplies and its reference clock, through RPMh (only with wcn_power=1) -------- */

/*
 * ath10k_hw_power_on(): the five supplies of the dt (the Mi 9 has four; "vdd-3.3-ch1" is not there), then the clocks
 * "cxo_ref_clk_pin" (RPMh's RF_CLK2, "rfclka2") and "qdss" (on since step 7). The supplies are RPMh's: an LDO is a
 * regulator resource of RPMh ("VRM": 4 in bits 19:16 of its address in the command database), with its voltage in mV
 * at +0, on/off at +4, the mode at +8 (Linux: qcom-rpmh-regulator.c). Step 2 found the four set to Android's voltages
 * and off; here each is voted on at the voltage and mode it already has, one after the other, and read back through the
 * PMIC arbiter: if one does not come up, or its voltage is not what it was, everything turned on is turned off and the
 * steps end. Nothing of this is done without wcn_power=1: this is the one step that could harm the radio if something
 * were wrong, and it waits for its owner's word.
 */
#define CMD_DB_POINTER_PHYS     0x0c3f0000UL    /* AOP's message RAM: at 0xc it says where the command database is */
#define CMD_DB_POINTER          0xc
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))   /* command slot set 0, an "active" one */
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
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define LDO_MODE_HPM            7               /* the PMIC's own value, which RPMh takes as it is */

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

static const char *const regulator_names[SUPPLIES] = { "ldoa1", "ldoa7", "ldoc2", "ldoc11" };  /* dt: rpmh-regulator-* */
static vm_offset_t cmd_db, rsc;
static uint32_t regulator_address[SUPPLIES], clock_address;
static bool supply_on[SUPPLIES], clock_on;

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	memcpy(id, name, strnlen(name, sizeof(id)));
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
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	say("wcn: %u: RPMh: no answer (register %#x = %#x)\n", step, reg, rd(TCS(reg)));
	return false;
}

/* One register write to RPMh, waited for (hakios.stfts.c's). */
static bool
rpmh_write(uint32_t address, uint32_t data)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			say("wcn: %u: RPMh: the command slots stay busy\n", step);
			lck_mtx_unlock(&hakios_rsc_lock);
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_CMD_MSGID(0)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
	wr(TCS(TCS_CMD_ADDR(0)), address);
	wr(TCS(TCS_CMD_DATA(0)), data);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 1);
	wr(TCS(TCS_CMD_ENABLE), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/* A supply as the PMIC has it: the voltage set (mV), the mode, enable (bit 7), status (bit 7: up). */
static bool
supply_read(unsigned s, uint16_t *mv, uint8_t *mode, uint8_t *enable, uint8_t *status)
{
	uint8_t voltage[2] = { 0 }, mode_enable[2] = { 0 };
	const char *problem;

	if (supply_channel[s] == ~0u) {
		say("wcn: %u: %s (%s): step 2 found no channel of the arbiter for it\n", step, supplies[s].supply, supplies[s].ldo);
		return false;
	}
	if ((problem = spmi_read(supply_channel[s], 0x08, status, 1)) != NULL ||
	    (problem = spmi_read(supply_channel[s], 0x40, voltage, 2)) != NULL ||
	    (problem = spmi_read(supply_channel[s], 0x45, mode_enable, 2)) != NULL) {
		say("wcn: %u: %s (%s): %s\n", step, supplies[s].supply, supplies[s].ldo, problem);
		return false;
	}
	*mv = (uint16_t)(voltage[1] << 8 | voltage[0]);
	*mode = mode_enable[0];
	*enable = mode_enable[1];
	return true;
}

/* A supply's voltage as set (0x40) and as the PMIC takes it to be valid (0x42), and its mode and status, said. */
static void
supply_look(unsigned s, const char *when)
{
	uint8_t set[2] = { 0 }, valid[2] = { 0 }, mode_enable[2] = { 0 }, status = 0;
	const char *problem;

	if ((problem = spmi_read(supply_channel[s], 0x40, set, 2)) != NULL ||
	    (problem = spmi_read(supply_channel[s], 0x42, valid, 2)) != NULL ||
	    (problem = spmi_read(supply_channel[s], 0x45, mode_enable, 2)) != NULL ||
	    (problem = spmi_read(supply_channel[s], 0x08, &status, 1)) != NULL) {
		say("wcn: %u: %s: %s: %s\n", step, supplies[s].supply, when, problem);
		return;
	}
	say("wcn: %u: %s: %s: set %u mV, valid %u mV, mode %#x, enable %#x, status %#x\n", step, supplies[s].supply, when,
	    set[1] << 8 | set[0], valid[1] << 8 | valid[0], mode_enable[0], mode_enable[1], status);
}

/* The reference clock off (ath10k_hw_power_off(): before the supplies). */
static void
clock_off(void)
{
	if (clock_on) {
		say("wcn: the radio's reference clock (rfclka2) off: %s\n", rpmh_write(clock_address + VRM_ENABLE, 0) ? "done" :
		    "RPMh DOES NOT ANSWER");
		clock_on = false;
	}
}

/* The supplies that were turned on, off again, the last first. */
static void
supplies_off(void)
{
	for (unsigned s = SUPPLIES; s-- > 0;) {
		uint16_t mv = 0;
		uint8_t mode = 0, enable = 0, status = 0;
		bool done;

		if (!supply_on[s]) {
			continue;
		}
		done = rpmh_write(regulator_address[s] + VRM_ENABLE, 0);
		supply_on[s] = false;
		delay_for_interval(2, NSEC_PER_MSEC);
		if (supply_read(s, &mv, &mode, &enable, &status)) {
			say("wcn: %s (%s) off%s: enable %#x, status %#x, %u mV\n", supplies[s].supply, supplies[s].ldo, done ? "" :
			    " (RPMh DOES NOT ANSWER)", enable, status, mv);
		}
	}
}

/* Everything step 9 turned on, off again, the clock first. */
static void
power_off(void)
{
	clock_off();
	supplies_off();
}

/* The command database and RPMh's addresses of the four regulators and the clock, once. */
static bool
power_prepare(void)
{
	if (clock_address != 0) {
		return true;
	}
	if (cmd_db == 0) {
		vm_offset_t pointer = ml_io_map(CMD_DB_POINTER_PHYS, 0x1000);
		uint32_t db = rd(pointer + CMD_DB_POINTER), size = rd(pointer + CMD_DB_POINTER + 4);

		if (db == 0 || size == 0 || size > CMD_DB_SIZE) {
			say("wcn: %u: no command database where AOP says (%#x, %#x bytes)\n", step, db, size);
			return false;
		}
		cmd_db = ml_io_map_unmappable(db, CMD_DB_SIZE, VM_WIMG_WCOMB);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	}
	for (unsigned s = 0; s < SUPPLIES; s++) {
		regulator_address[s] = rpmh_address(regulator_names[s]);
		if (regulator_address[s] == 0 || (regulator_address[s] >> 16 & 0xf) != 4) {
			say("wcn: %u: RPMh has no regulator \"%s\" (address %#x): nothing is voted\n", step, regulator_names[s],
			    regulator_address[s]);
			return false;
		}
	}
	clock_address = rpmh_address("rfclka2");
	if (clock_address == 0 || (clock_address >> 16 & 0xf) != 4) {
		say("wcn: %u: RPMh has no clock \"rfclka2\" (address %#x): nothing is voted\n", step, clock_address);
		clock_address = 0;
		return false;
	}
	say("wcn: %u: RPMh: ldoa1 %#x, ldoa7 %#x, ldoc2 %#x, ldoc11 %#x, rfclka2 %#x\n", step, regulator_address[0],
	    regulator_address[1], regulator_address[2], regulator_address[3], clock_address);
	return true;
}

/*
 * The four supplies on (ath10k_hw_power_on(): regulator_bulk_enable()), with wcn_power=1 only. All four are read
 * first; one set above what Android allows it stops everything, before any vote. Then each is voted on as Linux's
 * rpmh-regulator has it on this phone (its device tree: a fixed voltage each, regulator-initial-mode HPM): the
 * voltage it is set to when that is within Android's range, else the least of that range -- a supply nobody has
 * voted on is left lower by RPMh (logs/155501: L1 at 644 mV, mode LPM, where the boot loader had it at 752) --
 * and the high-power mode; each read back, and any that does not come up at the voltage voted turns all off again.
 */
static bool
supplies_on(void)
{
	uint16_t mv[SUPPLIES] = { 0 }, before[SUPPLIES] = { 0 };
	uint8_t mode[SUPPLIES] = { 0 };

	if (!option("wcn_power")) {
		say("wcn: %u: THE RADIO'S SUPPLIES ARE NOT TURNED ON: that wants wcn_power=1\n", step);
		return false;
	}
	if (!power_prepare()) {
		return false;
	}
	for (unsigned s = 0; s < SUPPLIES; s++) {
		uint8_t enable = 0, status = 0;

		if (!supply_read(s, &mv[s], &mode[s], &enable, &status)) {
			return false;
		}
		before[s] = mv[s];
		say("wcn: %u: %s (%s): %u mV, mode %#x, enable %#x, status %#x (Android: %u..%u mV)\n", step, supplies[s].supply,
		    supplies[s].ldo, mv[s], mode[s], enable, status, supplies[s].min_mv, supplies[s].max_mv);
		if (mv[s] > supplies[s].max_mv) {
			say("wcn: %u: %s IS SET ABOVE WHAT ANDROID ALLOWS IT: nothing is voted\n", step, supplies[s].supply);
			return false;
		}
		if (mv[s] < supplies[s].min_mv) {
			say("wcn: %u: %s is below Android's %u mV (nobody votes for it): it is voted at %u mV\n", step,
			    supplies[s].supply, supplies[s].min_mv, supplies[s].min_mv);
			mv[s] = supplies[s].min_mv;
		}
	}
	for (unsigned s = 0; s < SUPPLIES; s++) {
		uint16_t now_mv = 0;
		uint8_t now_mode = 0, enable = 0, status = 0;
		bool voted;
		unsigned ms;

		if (supply_on[s]) {
			continue;
		}
		say("wcn: %u: about to vote %s (%s) on, at %u mV, high-power mode (Android's)\n", step, supplies[s].supply,
		    supplies[s].ldo, mv[s]);
		/*
		 * (logs/160722: L1 came up at the 644 mV RPMh had left it at, not the 752 voted: what each vote does is
		 * said as it is read back, the voltage set (0x40) and the one the PMIC takes as valid (0x42) both)
		 */
		voted = rpmh_write(regulator_address[s] + VRM_VOLTAGE, mv[s]);
		delay_for_interval(5, NSEC_PER_MSEC);
		supply_look(s, voted ? "the voltage voted" : "THE VOLTAGE VOTE NOT TAKEN");
		voted = voted && rpmh_write(regulator_address[s] + VRM_MODE, LDO_MODE_HPM);
		delay_for_interval(2, NSEC_PER_MSEC);
		supply_look(s, voted ? "the mode voted" : "THE MODE VOTE NOT TAKEN");
		if (voted) {
			supply_on[s] = true;
			voted = rpmh_write(regulator_address[s] + VRM_ENABLE, 1);
		}
		/* up, and at the voltage voted: up to 50 ms; every change is said */
		for (ms = 0; ms < 50; ms++) {
			uint16_t was_mv = now_mv;
			uint8_t was_status = status;

			delay_for_interval(1, NSEC_PER_MSEC);
			if (!supply_read(s, &now_mv, &now_mode, &enable, &status)) {
				now_mv = was_mv;		/* (a read the bus dropped says nothing of the supply: the next one will) */
				status = was_status;
				continue;
			}
			if (ms == 0 || now_mv != was_mv || status != was_status) {
				say("wcn: %u:   +%u ms: %u mV, mode %#x, enable %#x, status %#x\n", step, ms + 1, now_mv, now_mode, enable,
				    status);
			}
			if ((status & 0x80) && now_mv >= mv[s]) {
				break;
			}
		}
		if (now_mv < mv[s]) {
			supply_look(s, "AT THE END OF THE WAIT");
		}
		/*
		 * Up, and at a voltage no higher than Android allows and no lower than RPMh had it before our vote (one
		 * step of 8 mV either way, the PMIC's rounding). logs/161137: RPMh takes the mode and the enable of L1
		 * and not the voltage voted (752), which stays at its own 644 mV; Android votes the same and reads
		 * nothing back (its rpmh-regulator, qcom,set 3), so what AOP sets is what Android's radio gets too.
		 */
		if (!voted || !(status & 0x80) || !(enable & 0x80) || now_mv + 8 < before[s] || now_mv > supplies[s].max_mv) {
			say("wcn: %u: %s (%s) DOES NOT COME UP AS IT SHOULD: voted %s, %u mV (voted %u, was %u), enable %#x, status %#x: "
			    "everything is turned off again\n", step, supplies[s].supply, supplies[s].ldo, voted ? "yes" : "NO", now_mv,
			    mv[s], before[s], enable, status);
			power_off();
			return false;
		}
		if (now_mv < mv[s]) {
			say("wcn: %u: %s (%s): RPMh HAS NOT TAKEN THE VOLTAGE VOTED: %u mV, not %u (as it was before the vote): "
			    "taken as it is\n", step, supplies[s].supply, supplies[s].ldo, now_mv, mv[s]);
		}
		say("wcn: %u: %s (%s) IS ON after %u ms: %u mV, mode %#x, status %#x\n", step, supplies[s].supply, supplies[s].ldo,
		    ms + 1, now_mv, now_mode, status);
	}
	return true;
}

/* The reference clock (RF_CLK2) on: after the supplies (ath10k_hw_power_on(): clk_bulk_prepare_enable()). */
static bool
clock_up(void)
{
	bool any = false;

	for (unsigned s = 0; s < SUPPLIES; s++) {
		any |= supply_on[s];
	}
	if (!any || !power_prepare()) {
		say("wcn: %u: the radio's reference clock is not turned on: its supplies are not\n", step);
		return false;
	}
	if (!clock_on && !rpmh_write(clock_address + VRM_ENABLE, 1)) {
		say("wcn: %u: RPMh DOES NOT TAKE \"rfclka2 on\"\n", step);
		return false;
	}
	clock_on = true;
	return true;
}

static bool
step_power(void)
{
	announce("vote the radio's four supplies on through RPMh, each at the voltage it is set to, then its reference clock "
	    "(RF_CLK2) -- with wcn_power=1 only");
	if (!option("wcn_power")) {
		say("wcn: 9: without wcn_power=1 the radio's supplies and its reference clock are left as they are (off): the next "
		    "step asks for mission mode without them\n");
		return true;
	}
	if (!supplies_on()) {
		return false;
	}
	if (!clock_up()) {
		power_off();
		return false;
	}
	say("wcn: 9: THE RADIO'S SUPPLIES AND ITS REFERENCE CLOCK ARE ON\n");
	return true;
}

/* ---- stage 2: the firmware's own service, WLFW (ath10k's qmi.c, by hand) --------------------------------------- */

/*
 * Steps 4 to 7 talk to the Wi-Fi firmware in the modem's WLAN PD, over the modem's QRTR (hakios.mpss.c: modem_step=10
 * modem_keep=1, so that the modem is up and stays so), in the order and with the values of ath10k's qmi.c
 * (ath10k_qmi_event_server_arrive(), ath10k_qmi_event_msa_ready()): which indications we want, what the host can do,
 * where the firmware's memory (MSA) is, that memory handed to the modem and the WLAN by TrustZone, "MSA ready", what
 * the chip and the firmware are; then the board's data file and the calibration report, after which the firmware
 * says "ready". Not yet: the configuration of the copy engines and the mode "mission" -- that wants the radio's
 * supplies on and its rings mapped (stage 3), and is ath10k's own business there.
 *
 * The messages' structures and tables are ath10k's (qmi_wlfw_v01.c, ISC), taken out of Linux when the kernel is
 * built (tools/mk-wlfw-ath10k.sh) and not kept in the repository; without them step 4 says so. The coding and the
 * transactions are ours (hakios.qmi.c).
 */
/* the firmware's memory (MSA), and TrustZone's names of its owners */
#define MSA_PHYS                0x8bc00000ull   /* dt: qcom,wlan-msa-fixed-region of qcom,icnss: pil_wlan_fw_region */
#define MSA_SIZE                0x00100000u     /* dt: qcom,wlan-msa-memory */
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define VMID_WLAN               0x18
#define VMID_WLAN_CE            0x19

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#if __has_include("hakios.wlfw_ath10k.h")

#include <sys/errno.h>
#define HAKIOS_QMI_NO_KERNEL_CONNECT
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include "hakios.wlfw_ath10k.h"

#define ATH10K_QMI_CLIENT_ID    0x4b4e454c      /* qmi.c */
#define WLFW_TIMEOUT_MS         5000

static struct qmi_handle handle;
static bool handle_open;
static struct {
	volatile bool server, gone, msa_ready, fw_ready;
	uint64_t began, msa_ready_at, fw_ready_at;
} wlfw;
static struct {
	uint64_t address;
	uint32_t size;
	bool secure, given;
} regions[QMI_WLFW_MAX_MEM_REG_V01];
static unsigned region_count;
static uint32_t board_id = 0xff;

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000000;
}

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = hakios_qmi_connect(qmi->sock, service->node, service->port);

	say("wcn: %u: WLFW is at node %u port %u (service %#x, version %u, instance %u)%s\n", step, service->node, service->port,
	    service->service, service->version, service->instance, error ? ": CANNOT BE CONNECTED TO" : "");
	wlfw.server = error == 0;
	return error;
}

static void
wlfw_del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	say("wcn: %u: WLFW AT NODE %u PORT %u HAS GONE\n", step, service->node, service->port);
	wlfw.gone = true;
}

static void
wlfw_msa_ready_ind(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	wlfw.msa_ready_at = now_ms();
	wlfw.msa_ready = true;
	say("wcn: %u: +%llu ms: THE FIRMWARE INDICATES \"MSA READY\"\n", step, wlfw.msa_ready_at - wlfw.began);
}

static void
wlfw_fw_ready_ind(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	wlfw.fw_ready_at = now_ms();
	wlfw.fw_ready = true;
	say("wcn: %u: +%llu ms: THE FIRMWARE INDICATES \"FW READY\"\n", step, wlfw.fw_ready_at - wlfw.began);
}

static void
wlfw_pin_result_ind(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    const void *decoded)
{
	const struct wlfw_pin_connect_result_ind_msg_v01 *pins = decoded;

	say("wcn: %u: the firmware tells of the radio's pins: power %#x (%s), phy io %#x (%s), rf %#x (%s)\n", step,
	    pins->pwr_pin_result, pins->pwr_pin_result_valid ? "given" : "-", pins->phy_io_pin_result,
	    pins->phy_io_pin_result_valid ? "given" : "-", pins->rf_pin_result, pins->rf_pin_result_valid ? "given" : "-");
}

static const struct qmi_ops wlfw_ops = { .new_server = wlfw_new_server, .del_server = wlfw_del_server };
static const struct qmi_msg_handler wlfw_handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, wlfw_fw_ready_ind_msg_v01_ei, sizeof(struct wlfw_fw_ready_ind_msg_v01),
	  wlfw_fw_ready_ind },
	{ QMI_INDICATION, QMI_WLFW_MSA_READY_IND_V01, wlfw_msa_ready_ind_msg_v01_ei, sizeof(struct wlfw_msa_ready_ind_msg_v01),
	  wlfw_msa_ready_ind },
	{ QMI_INDICATION, QMI_WLFW_PIN_CONNECT_RESULT_IND_V01, wlfw_pin_connect_result_ind_msg_v01_ei,
	  sizeof(struct wlfw_pin_connect_result_ind_msg_v01), wlfw_pin_result_ind },
	{ 0 },
};

static bool
wait_for(volatile bool *flag, uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !*flag && !wlfw.gone; i++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return *flag;
}

/*
 * One request and its reply, as each of qmi.c's goes; the reply's first field is the result. `tolerated`: an error the
 * firmware may give that qmi.c takes as success (0: none). A refused request is written out in full.
 */
static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply, uint16_t tolerated, bool quiet)
{
	const struct qmi_response_type_v01 *result = reply;
	static uint8_t bytes[QMI_HEADER_SIZE + 6400];
	struct qmi_txn txn;
	uint64_t began = now_ms();
	int ret;

	ret = qmi_txn_init(&handle, &txn, reply_ei, reply);
	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = hakios_qmi_txn_wait_ms(&txn, WLFW_TIMEOUT_MS);
		}
	}
	if (ret >= 0 && (result->result == QMI_RESULT_SUCCESS_V01 || (tolerated != 0 && result->error == tolerated))) {
		if (!quiet) {
			say("wcn: %u: %s (message %#x): %s, after %llu ms\n", step, what, message, result->result ==
			    QMI_RESULT_SUCCESS_V01 ? "success" : "refused with an error qmi.c lets pass", now_ms() - began);
		}
		return true;
	}
	if (ret < 0) {
		say("wcn: %u: %s (message %#x): NO ANSWER THAT CAN BE READ: %d, after %llu ms\n", step, what, message, ret,
		    now_ms() - began);
	} else {
		say("wcn: %u: %s (message %#x): THE FIRMWARE REFUSES: result %u, error %u (%#x), after %llu ms\n", step, what,
		    message, result->result, result->error, result->error, now_ms() - began);
	}
	ret = hakios_qmi_encode(QMI_REQUEST, (uint16_t)message, 0, request_ei, request, bytes, sizeof(bytes));
	for (int at = 0; at < ret && at < 96; at += 24) {
		char text[3 * 24 + 1] = "";

		for (int i = 0; i < 24 && at + i < ret; i++) {
			snprintf(text + 3 * i, 4, " %02x", bytes[at + i]);
		}
		say("wcn: %u:   the request, %d bytes, from %d:%s\n", step, ret, at, text);
	}
	return false;
}

/* 4: the modem's link, WLFW on it, our indications (ath10k_qmi_ind_register_send_sync_msg()) */
static bool
step_wlfw(void)
{
	struct wlfw_ind_register_req_msg_v01 request;
	struct wlfw_ind_register_resp_msg_v01 reply;
	const struct hakios_qrtr_ops *transport = NULL;
	uint32_t wait_s = 180, i;
	int ret;

	announce("wait for the modem's QRTR (modem_step=10 modem_keep=1), then for WLFW (0x45) on it, and say which of its "
	    "indications we want");
	PE_parse_boot_argn("wcn_wait", &wait_s, sizeof(wait_s));
	for (i = 0; i < wait_s * 10 && (transport = hakios_mpss_qrtr()) == NULL; i++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	if (transport == NULL) {
		say("wcn: 4: THE MODEM'S QRTR IS NOT THERE within %u s (wcn_wait): is the modem asked for (modem_step=10 modem_keep=1)?\n",
		    wait_s);
		return false;
	}
	say("wcn: 4: the modem's QRTR is there after %u.%u s\n", i / 10, i % 10);
	bzero(&wlfw, sizeof(wlfw));
	wlfw.began = now_ms();
	ret = hakios_qmi_handle_init_transport(&handle, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN, &wlfw_ops, wlfw_handlers,
	    transport);
	if (ret < 0) {
		say("wcn: 4: no handle on the modem's router: %d\n", ret);
		return false;
	}
	handle_open = true;
	ret = qmi_add_lookup(&handle, WLFW_SERVICE_ID_V01, WLFW_SERVICE_VERS_V01, 0);
	if (ret < 0 || !wait_for(&wlfw.server, 60000)) {
		say("wcn: 4: WLFW (%#x, version %u) DOES NOT APPEAR within 60 s (lookup: %d): the WLAN PD is not up\n",
		    WLFW_SERVICE_ID_V01, WLFW_SERVICE_VERS_V01, ret);
		return false;
	}
	say("wcn: 4: WLFW is there %llu ms after we looked\n", now_ms() - wlfw.began);
	bzero(&request, sizeof(request));
	bzero(&reply, sizeof(reply));
	request.client_id_valid = 1;
	request.client_id = ATH10K_QMI_CLIENT_ID;
	request.fw_ready_enable_valid = 1;
	request.fw_ready_enable = 1;
	request.msa_ready_enable_valid = 1;
	request.msa_ready_enable = 1;
	if (!ask("which indications we want", QMI_WLFW_IND_REGISTER_REQ_V01, WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_req_msg_v01_ei, &request, wlfw_ind_register_resp_msg_v01_ei, &reply, 0, false)) {
		return false;
	}
	if (reply.fw_status_valid) {
		say("wcn: 4:   the firmware's status word: %#llx%s\n", (unsigned long long)reply.fw_status,
		    reply.fw_status & QMI_WLFW_FW_READY_V01 ? " (READY ALREADY: a kernel before this one took it this far)" : "");
	} else {
		say("wcn: 4:   the firmware gives no status word\n");
	}
	return true;
}

/* 5: what the host can do (ath10k_qmi_host_cap_send_sync()), and where the firmware's memory is (..._msa_mem_info_...) */
static bool
step_msa_info(void)
{
	struct wlfw_host_cap_req_msg_v01 host;
	struct wlfw_host_cap_resp_msg_v01 host_reply;
	struct wlfw_msa_info_req_msg_v01 request;
	struct wlfw_msa_info_resp_msg_v01 reply;

	announce("tell the firmware what the host can do (no daemon), and where its memory is (0x8bc00000, 1 MB): it answers "
	    "with the regions it wants of that");
	bzero(&host, sizeof(host));
	bzero(&host_reply, sizeof(host_reply));
	host.daemon_support_valid = 1;
	host.daemon_support = 0;
	if (!ask("what the host can do", QMI_WLFW_HOST_CAP_REQ_V01, WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_host_cap_req_msg_v01_ei, &host, wlfw_host_cap_resp_msg_v01_ei, &host_reply, QMI_ERR_NOT_SUPPORTED_V01, false)) {
		return false;
	}
	bzero(&request, sizeof(request));
	bzero(&reply, sizeof(reply));
	request.msa_addr = MSA_PHYS;
	request.size = MSA_SIZE;
	if (!ask("where the firmware's memory is", QMI_WLFW_MSA_INFO_REQ_V01, WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_msa_info_req_msg_v01_ei, &request, wlfw_msa_info_resp_msg_v01_ei, &reply, 0, false)) {
		return false;
	}
	if (reply.mem_region_info_len == 0 || reply.mem_region_info_len > QMI_WLFW_MAX_MEM_REG_V01) {
		say("wcn: 5: THE FIRMWARE NAMES %u REGIONS of its memory\n", reply.mem_region_info_len);
		return false;
	}
	region_count = reply.mem_region_info_len;
	for (unsigned r = 0; r < region_count; r++) {
		const struct wlfw_memory_region_info_s_v01 *region = &reply.mem_region_info[r];

		say("wcn: 5:   region %u: %#llx, %#x bytes, %s\n", r, (unsigned long long)region->region_addr, region->size,
		    region->secure_flag ? "secure" : "not secure");
		if (region->region_addr < MSA_PHYS || region->size > MSA_SIZE ||
		    region->region_addr + region->size > MSA_PHYS + MSA_SIZE) {
			say("wcn: 5: THAT REGION IS NOT WITHIN THE MEMORY WE GAVE: nothing is handed over\n");
			region_count = 0;
			return false;
		}
		regions[r].address = region->region_addr;
		regions[r].size = region->size;
		regions[r].secure = region->secure_flag != 0;
		regions[r].given = false;
	}
	return true;
}

/*
 * 6: the regions handed by TrustZone from us to the modem and the WLAN (ath10k_qmi_map_msa_permission(): HLOS ->
 * MSS_MSA and WLAN, and WLAN_CE where a region is not secure), then "MSA ready" (..._msa_ready_send_sync_msg()) and
 * what the chip and the firmware are (..._cap_send_sync_msg()). The regions are not given back while the modem may
 * use them: the phone is restarted between runs anyway.
 */
static bool
step_msa(void)
{
	struct wlfw_msa_ready_req_msg_v01 ready = { 0 };
	struct wlfw_msa_ready_resp_msg_v01 ready_reply;
	struct wlfw_cap_req_msg_v01 what = { 0 };
	static struct wlfw_cap_resp_msg_v01 cap;

	announce("ask TrustZone to hand the firmware's memory regions from us to the modem and the WLAN, tell the firmware "
	    "\"MSA ready\", and ask what the chip and the firmware are");
	delay_for_interval(20, NSEC_PER_MSEC);         /* (qmi.c: between the answer and the XPU's update, for SDM845) */
	for (unsigned r = 0; r < region_count; r++) {
		const uint32_t hlos = VMID_HLOS, to[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
		uint64_t answer = ~0ull;
		int error;

		say("wcn: 6: about to ask TrustZone to hand %#llx (%#x bytes) from HLOS to MSS_MSA, WLAN%s\n",
		    (unsigned long long)regions[r].address, regions[r].size, regions[r].secure ? "" : " and WLAN_CE");
		error = hakios_mpss_assign(regions[r].address, regions[r].size, &hlos, 1, to, regions[r].secure ? 2 : 3, &answer);
		if (error != 0) {
			say("wcn: 6: TRUSTZONE DOES NOT HAND THE REGION OVER: error %d, its answer %#llx\n", error, answer);
			return false;
		}
		regions[r].given = true;
		say("wcn: 6: region %u is the modem's and the WLAN's now\n", r);
	}
	bzero(&ready_reply, sizeof(ready_reply));
	if (!ask("MSA ready", QMI_WLFW_MSA_READY_REQ_V01, WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN, wlfw_msa_ready_req_msg_v01_ei,
	    &ready, wlfw_msa_ready_resp_msg_v01_ei, &ready_reply, 0, false)) {
		return false;
	}
	bzero(&cap, sizeof(cap));
	if (!ask("what the chip and the firmware are", QMI_WLFW_CAP_REQ_V01, WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_cap_req_msg_v01_ei, &what, wlfw_cap_resp_msg_v01_ei, &cap, 0, false)) {
		return false;
	}
	board_id = cap.board_info_valid ? cap.board_info.board_id : 0xff;
	say("wcn: 6:   CHIP ID %#x, family %#x (%s); BOARD ID %#x (%s); soc id %#x (%s)\n", cap.chip_info.chip_id,
	    cap.chip_info.chip_family, cap.chip_info_valid ? "given" : "not given", board_id, cap.board_info_valid ? "given" :
	    "not given: ath10k takes 0xff", cap.soc_info.soc_id, cap.soc_info_valid ? "given" : "not given");
	say("wcn: 6:   firmware version %#x, built \"%s\" (%s); BUILD ID \"%s\" (%s); macs %u (%s)\n",
	    cap.fw_version_info.fw_version, cap.fw_version_info.fw_build_timestamp, cap.fw_version_info_valid ? "given" :
	    "not given", cap.fw_build_id, cap.fw_build_id_valid ? "given" : "not given", cap.num_macs, cap.num_macs_valid ?
	    "given" : "not given");
	if (!wait_for(&wlfw.msa_ready, 5000)) {
		say("wcn: 6: no \"MSA ready\" from the firmware within 5 s (ath10k sends the board's data on it)\n");
	}
	return true;
}

/*
 * 7: the board's data (ath10k_qmi_bdf_dnld_send_sync(): pieces of 6 KB, file id 0; a CRC complaint on the last piece
 * is let pass), the file Android's would send for that board id (fw/image/bdwlan.bXX of the phone's own
 * firmware), the calibration report (none), and the wait for "FW READY".
 */
static bool
step_board(void)
{
	static struct wlfw_bdf_download_req_msg_v01 request;
	struct wlfw_bdf_download_resp_msg_v01 reply;
	struct wlfw_cal_report_req_msg_v01 report;
	struct wlfw_cal_report_resp_msg_v01 report_reply;
	char name[24];
	const uint8_t *data;
	uint64_t size = 0, began;
	uint32_t left, pieces = 0, ready_s = 15;

	if (board_id < 0xff) {
		snprintf(name, sizeof(name), "bdwlan.b%02x", board_id);
	} else if (board_id == 0xff) {
		strlcpy(name, "bdwlan.bin", sizeof(name));
	} else {
		snprintf(name, sizeof(name), "bdwlan.%03x", board_id);
	}
	announce("turn the WLAN's debug clock (\"qdss\") on through the always-on processor, send the board's data file for "
	    "that board id in pieces, the calibration report, and wait for \"FW READY\"");
	/*
	 * The clock first (Linux's dt: clock-names "cxo_ref_clk_pin", "qdss" of wifi@18800000, the second <&aoss_qmp>:
	 * qcom_aoss.c's "{class: clock, res: qdss, val: 1}"). Without it, on the phone (2026-11-06, logs/220547-run.log),
	 * the firmware died 13 ms after the calibration report, "EX:wlan_process:0x1:WLAN RT:0x74:PC=0xb0193a20": there
	 * wlanmdsp.mbn writes 0xc5acce55 to a CoreSight lock (+0xfb0) of the WCSS's debug block and reads its status
	 * (+0xfb4), which a block without its clock does not answer. wcn_qdss_off=1: not sent.
	 */
	if (!option("wcn_qdss_off") && !hakios_mpss_aop("{class: clock, res: qdss, val: 1}")) {
		say("wcn: 7: THE ALWAYS-ON PROCESSOR DOES NOT TAKE \"qdss on\"\n");
		return false;
	}
	data = hakios_image_file("fw/image", name, &size, NULL);
	if (data == NULL || size == 0 || size > 512 * 1024) {
		say("wcn: 7: fw/image/%s is not in the image (MODEM=1 FLASHBOOT=1 userland/build.sh), or is no such file (%llu bytes)\n",
		    name, size);
		return false;
	}
	say("wcn: 7: fw/image/%s: %llu bytes, beginning %02x %02x %02x %02x\n", name, size, data[0], data[1], data[2], data[3]);
	began = now_ms();
	bzero(&request, sizeof(request));
	for (left = (uint32_t)size; left > 0; pieces++) {
		request.valid = 1;
		request.file_id_valid = 1;
		request.file_id = 0;
		request.total_size_valid = 1;
		request.total_size = (uint32_t)size;
		request.seg_id_valid = 1;
		request.data_valid = 1;
		request.end_valid = 1;
		request.end = left <= QMI_WLFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, data, request.data_len);
		bzero(&reply, sizeof(reply));
		if (!ask("the board's data", QMI_WLFW_BDF_DOWNLOAD_REQ_V01, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_bdf_download_req_msg_v01_ei, &request, wlfw_bdf_download_resp_msg_v01_ei, &reply,
		    request.end ? QMI_ERR_MALFORMED_MSG_V01 : 0, true)) {
			say("wcn: 7: THE BOARD'S DATA IS REFUSED AT PIECE %u (%u bytes, %u still to go)\n", pieces, request.data_len, left);
			return false;
		}
		if (request.end && reply.resp.result != QMI_RESULT_SUCCESS_V01) {
			say("wcn: 7: the last piece: the firmware's CRC check complains (error %u), which ath10k lets pass\n",
			    reply.resp.error);
		}
		left -= request.data_len;
		data += request.data_len;
		request.seg_id++;
	}
	say("wcn: 7: THE BOARD'S DATA IS TAKEN: %u pieces in %llu ms\n", pieces, now_ms() - began);
	bzero(&report, sizeof(report));
	bzero(&report_reply, sizeof(report_reply));
	if (!ask("the calibration report (none)", QMI_WLFW_CAL_REPORT_REQ_V01, WLFW_CAL_REPORT_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_cal_report_req_msg_v01_ei, &report, wlfw_cal_report_resp_msg_v01_ei, &report_reply, 0, false)) {
		return false;
	}
	PE_parse_boot_argn("wcn_ready", &ready_s, sizeof(ready_s));
	if (!wait_for(&wlfw.fw_ready, ready_s * 1000)) {
		say("wcn: 7: NO \"FW READY\" within %u s of the report (wcn_ready)\n", ready_s);
		return false;
	}
	say("wcn: 7: THE FIRMWARE IS READY, %llu ms after we first looked for it: what follows (the copy engines' "
	    "configuration, mission mode) is stage 3\n", wlfw.fw_ready_at - wlfw.began);
	return true;
}

/*
 * 10: what ath10k_snoc_wlan_enable() tells the firmware once the radio is powered (step 9): the copy engines'
 * configuration -- snoc.c's target_ce_config_wlan, target_service_to_ce_map_wlan and target_shadow_reg_cfg_map, taken
 * out of Linux with the message tables -- then the mode "mission" (ath10k_qmi_wlan_enable(): no host version, hw_debug
 * 0). Then the firmware is watched for wcn_watch=S seconds (default 10): if the WLAN PD dies, WLFW goes and the modem's
 * service registry says why (hakios.mpss.c). With wcn_ce_peek=1 the copy engines' registers (0x18800000 + 0x240000, a
 * page each) are read after that, which is what ath10k does next; on the SM7325 phone a read there before mission
 * mode ended the kernel, so not before. On the way out the mode is set "off" again (ath10k_qmi_wlan_disable()).
 */
#define CE_PHYS                 0x18a40000UL    /* dt: reg "membase" 0x18800000 of qcom,icnss, + hw.c's ce0_base_address */
#define COUNT_OF(a)             (sizeof(a) / sizeof((a)[0]))
static bool mission_on;

static void
ce_peek(void)
{
	vm_offset_t ce = ml_io_map(CE_PHYS, 12 * 0x1000);

	for (unsigned n = 0; n < 12; n++) {
		vm_offset_t r = ce + 0x1000 * n;

		/* hw.c's wcn3990_ce_regs: source ring base, size; destination ring base, size; write indexes; read indexes */
		say("wcn: 10: CE%u: source ring %#x%08x size %u, destination ring %#x%08x size %u; written %u/%u, read %u/%u, "
		    "interrupts %#x\n", n, rd(r + 0x04), rd(r + 0x00), rd(r + 0x08), rd(r + 0x10), rd(r + 0x0c), rd(r + 0x14),
		    rd(r + 0x3c), rd(r + 0x40), rd(r + 0x44), rd(r + 0x48), rd(r + 0x2c));
	}
}

static bool
step_mission(void)
{
	static struct wlfw_wlan_cfg_req_msg_v01 cfg;
	struct wlfw_wlan_cfg_resp_msg_v01 cfg_reply;
	struct wlfw_wlan_mode_req_msg_v01 mode;
	struct wlfw_wlan_mode_resp_msg_v01 mode_reply;
	uint32_t watch_s = 10, s;

	announce("give the firmware the copy engines' configuration (ath10k's snoc.c), ask for mission mode, and watch it");
	if (!clock_on) {
		say("wcn: 10: the radio is NOT POWERED (no wcn_power=1): this is what the firmware does without its supplies\n");
	}
	bzero(&cfg, sizeof(cfg));
	cfg.tgt_cfg_valid = 1;
	cfg.tgt_cfg_len = COUNT_OF(target_ce_config_wlan) < QMI_WLFW_MAX_NUM_CE_V01 ? COUNT_OF(target_ce_config_wlan) :
	    QMI_WLFW_MAX_NUM_CE_V01;
	for (unsigned i = 0; i < cfg.tgt_cfg_len; i++) {
		cfg.tgt_cfg[i].pipe_num = target_ce_config_wlan[i].pipenum;
		cfg.tgt_cfg[i].pipe_dir = target_ce_config_wlan[i].pipedir;
		cfg.tgt_cfg[i].nentries = target_ce_config_wlan[i].nentries;
		cfg.tgt_cfg[i].nbytes_max = target_ce_config_wlan[i].nbytes_max;
		cfg.tgt_cfg[i].flags = target_ce_config_wlan[i].flags;
	}
	cfg.svc_cfg_valid = 1;
	cfg.svc_cfg_len = COUNT_OF(target_service_to_ce_map_wlan) < QMI_WLFW_MAX_NUM_SVC_V01 ?
	    COUNT_OF(target_service_to_ce_map_wlan) : QMI_WLFW_MAX_NUM_SVC_V01;
	for (unsigned i = 0; i < cfg.svc_cfg_len; i++) {
		cfg.svc_cfg[i].service_id = target_service_to_ce_map_wlan[i].service_id;
		cfg.svc_cfg[i].pipe_dir = target_service_to_ce_map_wlan[i].pipedir;
		cfg.svc_cfg[i].pipe_num = target_service_to_ce_map_wlan[i].pipenum;
	}
	cfg.shadow_reg_valid = 1;
	cfg.shadow_reg_len = COUNT_OF(target_shadow_reg_cfg_map) < QMI_WLFW_MAX_NUM_SHADOW_REG_V01 ?
	    COUNT_OF(target_shadow_reg_cfg_map) : QMI_WLFW_MAX_NUM_SHADOW_REG_V01;
	for (unsigned i = 0; i < cfg.shadow_reg_len; i++) {
		cfg.shadow_reg[i].id = target_shadow_reg_cfg_map[i].ce_id;
		cfg.shadow_reg[i].offset = target_shadow_reg_cfg_map[i].reg_offset;
	}
	say("wcn: 10: %u copy engines, %u services, %u shadow registers\n", cfg.tgt_cfg_len, cfg.svc_cfg_len, cfg.shadow_reg_len);
	bzero(&cfg_reply, sizeof(cfg_reply));
	if (!ask("the copy engines' configuration", QMI_WLFW_WLAN_CFG_REQ_V01, WLFW_WLAN_CFG_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_wlan_cfg_req_msg_v01_ei, &cfg, wlfw_wlan_cfg_resp_msg_v01_ei, &cfg_reply, 0, false)) {
		return false;
	}
	bzero(&mode, sizeof(mode));
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = QMI_WLFW_MISSION_V01;
	mode.hw_debug_valid = 1;
	mode.hw_debug = 0;
	if (!ask("mission mode", QMI_WLFW_WLAN_MODE_REQ_V01, WLFW_WLAN_MODE_REQ_MSG_V01_MAX_MSG_LEN, wlfw_wlan_mode_req_msg_v01_ei,
	    &mode, wlfw_wlan_mode_resp_msg_v01_ei, &mode_reply, 0, false)) {
		return false;
	}
	mission_on = true;
	PE_parse_boot_argn("wcn_watch", &watch_s, sizeof(watch_s));
	for (s = 0; s < watch_s && !wlfw.gone; s++) {
		sleep_ms(1000);
	}
	if (wlfw.gone) {
		say("wcn: 10: THE FIRMWARE WENT %u s AFTER MISSION MODE (WLFW gone; the modem's registry says why)\n", s);
		mission_on = false;
		return false;
	}
	say("wcn: 10: THE FIRMWARE IS IN MISSION MODE and still there after %u s%s\n", watch_s, clock_on ? "" :
	    " (unpowered)");
	space_faults();
	if (option("wcn_ce_peek")) {
		ce_peek();
	}
	return true;
}

static void
wlfw_close(void)
{
	if (mission_on && handle_open && !wlfw.gone) {
		struct wlfw_wlan_mode_req_msg_v01 mode = { .mode = QMI_WLFW_OFF_V01 };
		struct wlfw_wlan_mode_resp_msg_v01 reply;

		bzero(&reply, sizeof(reply));
		ask("the mode \"off\"", QMI_WLFW_WLAN_MODE_REQ_V01, WLFW_WLAN_MODE_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_wlan_mode_req_msg_v01_ei, &mode, wlfw_wlan_mode_resp_msg_v01_ei, &reply, 0, false);
	}
	mission_on = false;
	if (handle_open) {
		qmi_handle_release(&handle);
		handle_open = false;
	}
}

#define WLFW_BUILT              1
#else
#define WLFW_BUILT              0
static bool
step_wlfw(void)
{
	say("wcn: 4: WLFW IS NOT BUILT IN: ath10k's message descriptions were not there when this kernel was built "
	    "(tools/mk-wlfw-ath10k.sh, then build again)\n");
	return false;
}
static bool step_msa_info(void) { return false; }
static bool step_msa(void) { return false; }
static bool step_board(void) { return false; }
static bool step_mission(void) { return false; }
static void wlfw_close(void) {}
#endif

/* ---- stage 3: Linux's ath10k (tools/mk-ath10k.sh) on WLAN, with wcn_lx=1 ----------------------------------------- */

/*
 * The driver's object is linked into the kernel when CONFIG_HAKIOS_WLAN and this driver are built and
 * tools/mk-ath10k.sh has made it (xnu-build.sh); its side of the kernel is hakios.lx.c, which asks the phone's
 * radio driver what it knows of the phone through the hakios_wifi_*() of sys/hakios.input.h -- on the SM7325 phone
 * hakios.wifi.c, here these. And three calls of sys/hakios.lx.h that only this radio's driver makes, since ath10k's
 * bus powers the radio itself: its supplies, its clock, and the MSA's hand-over.
 *
 * With wcn_lx=1 the kernel does steps 1 to 3, waits for the modem's QRTR (modem_step=10 modem_keep=1), turns the
 * WLAN's debug clock on (as step 7 does, without which the WLAN PD dies), and hands the rest to ath10k (lx_start()):
 * its qmi.c has the exchange with WLFW that steps 4 to 7 have by hand (the kernel's own WLFW client is not opened),
 * snoc.c powers the radio -- with wcn_power=1 only: without it the kernel refuses, and the driver stops there -- and
 * asks for mission mode, core.c brings the copy engines, HTC and WMI up and registers the radio. The layer's options
 * are the boot-args of the same names as on the SM7325 phone (wifi_scan=1, wifi_join=1, wifi_quiet=1 ...). When
 * lx_start() returns and nothing is to stay up, the supplies, the clock and the SMMU are put back as they were.
 */
#define LX_STEP                 11              /* (how the log names what ath10k does) */

static int
say_list(const char *format, va_list list)
{
	char line[400];
	int length = vsnprintf(line, sizeof(line), format, list);

	say("%s", line);
	return length;
}

int
hakios_wifi_say(const char *format, ...)
{
	va_list list;
	int length;

	va_start(list, format);
	length = say_list(format, list);
	va_end(list);
	return length;
}

int
hakios_wifi_tell(const char *format, ...)
{
	va_list list;
	int length;

	va_start(list, format);
	length = say_list(format, list);
	va_end(list);
	return length;
}

uint32_t
hakios_wifi_option(const char *name)
{
	uint32_t value = 0;

	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

bool
hakios_wifi_dsp_is_up(void)
{
	return hakios_mpss_qrtr() != NULL;
}

bool
hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	bool ok = hakios_wcn3990_radio_map(physical, size, address);

	say("wcn: %u: %llu KB of ours shown to the radio%s%#llx\n", LX_STEP, (unsigned long long)(size >> 10), ok ? " at " :
	    ": NOT DONE, ", ok ? *address : 0ull);
	return ok;
}

/* (the copy engines' interrupts are not attached: wifi/lx/ath10k/lx_snoc.c looks at the engines every millisecond) */
bool
hakios_wifi_radio_msi(__unused unsigned long long *address, __unused uint32_t *data, __unused uint32_t *first_spi)
{
	return false;
}

/*
 * The radio's own address: boot-arg wcn_mac, six bytes in hexadecimal with colons between (Android keeps the phone's in
 * its storage; it is in no source here). Without one, an address is made that stays this phone's: SHA-256 of the
 * chip's serial number (socinfo, SMEM item 137, as Linux's socinfo.c reads it) after a word of ours, its first six
 * bytes as a locally administered address (02 in the first byte's low bits) -- the serial cannot be read back out of
 * it; only if the serial is not there, a random one for this boot. The firmware's is not taken: en0 is made with it
 * when the kernel starts (with
 * wifi_station=1), before the radio is up -- configd and wifid look for a Wi-Fi interface once, when they start, and
 * one that turns up with ath10k ten seconds later gets no network service (logs/175357: net_test asked for DHCP itself,
 * and Settings showed the network joined with a spinner, not a check mark). The log says which, not the address.
 */
bool
hakios_wifi_radio_address(uint8_t *address)
{
	static uint8_t made[6];
	static bool decided, given;
	char text[32] = "";
	uint8_t bytes[6];
	unsigned count = 0, digits = 0, value = 0;
	bool good = true;

	if (decided) {
		memcpy(address, made, sizeof(made));
		return true;
	}
	if (PE_parse_boot_arg_str("wcn_mac", text, sizeof(text)) && text[0] != 0) {
		for (const char *c = text;; c++) {
			int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' &&
			    *c <= 'F' ? *c - 'A' + 10 : -1;

			if (digit >= 0 && digits < 2) {
				value = value << 4 | (unsigned)digit;
				digits++;
			} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
				bytes[count++] = (uint8_t)value;
				digits = value = 0;
			} else {
				good = false;
			}
			if (*c == 0 || !good) {
				break;
			}
		}
		given = good && count == 6 && !(bytes[0] & 1) && (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] |
		    bytes[5]) != 0;
		if (given) {
			memcpy(made, bytes, sizeof(bytes));
		}
		say("wcn: the radio's address: wcn_mac was given, %s\n", given ? "six bytes: it is the radio's for this run" :
		    "WHICH IS NOT A STATION'S ADDRESS: one is made instead");
	}
	if (!given) {
		uint32_t size = 0, at = smem_item(SMEM_SOCINFO, &size), serial = 0;

		if (at != 0 && size >= SOCINFO_SERIAL + 4 && smem_peek(at + SOCINFO_SERIAL, &serial, sizeof(serial)) && serial != 0) {
			static const char word[] = "hakios cepheus wlan";
			uint8_t digest[SHA256_DIGEST_LENGTH];
			SHA256_CTX context;

			SHA256_Init(&context);
			SHA256_Update(&context, word, sizeof(word) - 1);
			SHA256_Update(&context, &serial, sizeof(serial));
			SHA256_Final(digest, &context);
			memcpy(made, digest, sizeof(made));
			say("wcn: the radio's address: made from the chip's serial number (the same at every start; wcn_mac= instead)\n");
		} else {
			read_random(made, sizeof(made));
			say("wcn: the radio's address: made for this boot (THE CHIP'S SERIAL NUMBER IS NOT IN SMEM: item %u, %u bytes)\n",
			    SMEM_SOCINFO, size);
		}
		made[0] = (uint8_t)((made[0] & 0xfc) | 0x02);   /* (unicast, locally administered) */
	}
	decided = true;
	memcpy(address, made, sizeof(made));
	return true;
}

/*
 * The network to join (wifi_join=1): fw/network of the small image, the name on its first line and the passphrase on
 * its second, as on the SM7325 phone (userland/build.sh takes it from fw-wifi/network, which is in no repository).
 */
bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	const uint8_t *file = hakios_image_file("fw", "network", &size, NULL), *end = file + size, *line = file;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		say("wcn: %u: which network to join: none was given (fw/network of the small image)\n", LX_STEP);
		return false;
	}
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	say("wcn: %u: which network to join: a name of %u bytes and a passphrase of %u bytes\n", LX_STEP, lengths[0], lengths[1]);
	if (lengths[0] == 0 || lengths[0] > 32 || lengths[1] < 8 || lengths[1] > 63) {
		return false;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return true;
}

/*
 * What the firmware's own file (firmware-N.bin of Linux's firmware tree) says to ath10k: not the firmware -- that is
 * the modem's wlanmdsp.mbn -- but what it can do (core.c: ath10k_core_fetch_firmware_api_n()). Linux's tree has one for
 * the WCN3990 (ath10k/WCN3990/hw1.0/firmware-5.bin); it is not here, and what it holds is made here, from what ath10k
 * needs of this firmware: no BMI (the modem loads it), management frames sent by reference, one channel's information
 * per channel; WMI and HTT in their TLV forms. "QCA-ATH10K" and its NUL, padded to four, then (id, length, data padded
 * to four) after another. A file fw/image/firmware-5.bin in the small image is taken instead.
 */
#define FW_IE_FW_VERSION        0
#define FW_IE_FEATURES          2
#define FW_IE_WMI_OP_VERSION    5
#define FW_IE_HTT_OP_VERSION    6
#define FW_FEATURE_MGMT_TX_BY_REF               18
#define FW_FEATURE_NON_BMI                      19
#define FW_FEATURE_SINGLE_CHAN_INFO_PER_CHANNEL 20
#define WMI_OP_VERSION_TLV      4
#define HTT_OP_VERSION_TLV      3

static uint8_t features_file[128];
static uint32_t features_length;

static void
features_ie(uint32_t id, const void *data, uint32_t length)
{
	memcpy(features_file + features_length, &id, 4);
	memcpy(features_file + features_length + 4, &length, 4);
	memcpy(features_file + features_length + 8, data, length);
	features_length += 8 + ((length + 3) & ~3u);
}

static const uint8_t *
features(uint64_t *size)
{
	static const char version[] = "WLAN.HL.3.0 (features made by hakios.wcn3990.c)";
	uint8_t bits[3] = { 0 };
	uint32_t wmi = WMI_OP_VERSION_TLV, htt = HTT_OP_VERSION_TLV;

	if (features_length == 0) {
		bzero(features_file, sizeof(features_file));
		memcpy(features_file, "QCA-ATH10K", 11);
		features_length = 12;
		features_ie(FW_IE_FW_VERSION, version, sizeof(version) - 1);
		bits[FW_FEATURE_MGMT_TX_BY_REF / 8] |= 1u << FW_FEATURE_MGMT_TX_BY_REF % 8;
		bits[FW_FEATURE_NON_BMI / 8] |= 1u << FW_FEATURE_NON_BMI % 8;
		bits[FW_FEATURE_SINGLE_CHAN_INFO_PER_CHANNEL / 8] |= 1u << FW_FEATURE_SINGLE_CHAN_INFO_PER_CHANNEL % 8;
		features_ie(FW_IE_FEATURES, bits, sizeof(bits));
		features_ie(FW_IE_WMI_OP_VERSION, &wmi, 4);
		features_ie(FW_IE_HTT_OP_VERSION, &htt, 4);
	}
	*size = features_length;
	return features_file;
}

/*
 * ath10k asks for files by the names of Linux's firmware tree (ath10k/WCN3990/hw1.0/...). Two are there: board.bin,
 * the board's data -- the phone's own fw/image/bdwlan.bin, which is what Android sends for a firmware that gives no
 * board id (step 6: none) -- and firmware-5.bin (above). The others it asks for first, or can do without (board-2.bin,
 * firmware-6.bin, a calibration file), are not.
 */
const uint8_t *
hakios_wifi_driver_file(const char *name, uint64_t *size)
{
	const char *last = strrchr(name, '/');
	const uint8_t *data = NULL;

	last = last == NULL ? name : last + 1;
	if (strcmp(last, "board.bin") == 0) {
		data = hakios_image_file("fw/image", "bdwlan.bin", size, NULL);
	} else if (strcmp(last, "firmware-5.bin") == 0) {
		data = hakios_image_file("fw/image", "firmware-5.bin", size, NULL);
		if (data == NULL) {
			data = features(size);
		}
	}
	say("wcn: %u: the driver asks for %s: %s (%llu bytes)\n", LX_STEP, name, data != NULL ? "given" : "not there",
	    data != NULL ? (unsigned long long)*size : 0ull);
	return data;
}

/* ---- ... and what only this radio's driver asks (sys/hakios.lx.h) ---- */

int
lx_os_radio_supplies(int on)
{
	step = LX_STEP;
	if (!on) {
		supplies_off();
		return 0;
	}
	return supplies_on() ? 0 : EPERM;
}

int
lx_os_radio_clocks(int on)
{
	step = LX_STEP;
	if (!on) {
		clock_off();
		return 0;
	}
	return clock_up() ? 0 : EIO;
}

/*
 * A region of the MSA handed by TrustZone from us to the modem and the WLAN (and the WLAN's copy engines), as step 6
 * does; anything not inside the MSA is refused. It is not handed back: the modem may still be using it, and the phone
 * is restarted between runs (ath10k only warns when that is refused).
 */
int
lx_os_radio_hand_over(unsigned long long address, unsigned long long size, int to_radio, int with_ce)
{
	const uint32_t hlos = VMID_HLOS, to[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
	uint64_t answer = ~0ull;
	int error;

	if (size == 0 || address < MSA_PHYS || address + size > MSA_PHYS + MSA_SIZE || address + size < address) {
		say("wcn: %u: TrustZone is asked to hand %#llx (%llu bytes), which is not of the MSA: REFUSED\n", LX_STEP, address,
		    size);
		return EPERM;
	}
	if (!to_radio) {
		say("wcn: %u: %#llx is not handed back to us: the modem may still use it (the phone is restarted between runs)\n",
		    LX_STEP, address);
		return EPERM;
	}
	say("wcn: %u: about to ask TrustZone to hand %#llx (%#llx bytes) from HLOS to MSS_MSA, WLAN%s\n", LX_STEP, address, size,
	    with_ce ? " and WLAN_CE" : "");
	error = hakios_mpss_assign(address, size, &hlos, 1, to, with_ce ? 3 : 2, &answer);
	say("wcn: %u: %s (error %d, its answer %#llx)\n", LX_STEP, error == 0 ? "handed over" : "TRUSTZONE DOES NOT HAND IT OVER",
	    error, answer);
	return error;
}

/* wcn_lx=1, after steps 1 to 3: the modem's QRTR, the debug clock, then ath10k. */
/*
 * wifi_station=1: the network of fw/network joined the way anybody joins one, by asking the station
 * (hakios.sta.c), as the SM7325's wifi=1 does (hakios.wifi.c, join_given()); Darwin's Wi-Fi side may ask for
 * another later, or for none, and configd's "Wi-Fi" service then configures en0 by DHCP. wifi_file=0: none.
 */
static void
lx_join_given(void)
{
	static struct hakios_sta_join what;
	struct hakios_sta_state state;
	unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase), waited;
	uint32_t given = 1;
	int error;

	if (PE_parse_boot_argn("wifi_file", &given, sizeof(given)) && given == 0) {
		say("wcn: %u: the network of the file is not joined (wifi_file=0): the radio waits to be asked\n", LX_STEP);
		return;
	}
	bzero(&what, sizeof(what));
	if (!hakios_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
		say("wcn: %u: no network to join (fw/network): the radio waits to be asked\n", LX_STEP);
		bzero(&what, sizeof(what));
		return;
	}
	what.ssid_length = (uint8_t)name_length;
	what.passphrase_length = (uint8_t)passphrase_length;
	what.security = HAKIOS_STA_WPA2_PSK;
	error = hakios_sta_ask_join_standing(&what);
	bzero(&what, sizeof(what));
	for (waited = 0; error == 0 && hakios_sta_busy() && waited < 300; waited++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	bzero(&state, sizeof(state));
	hakios_sta_state(&state);
	if (state.phase == HAKIOS_STA_JOINED) {
		say("wcn: %u: THE NETWORK OF THE FILE IS JOINED through the station, %u.%u s after it was asked\n", LX_STEP,
		    waited / 10, waited % 10);
	} else {
		say("wcn: %u: THE NETWORK OF THE FILE IS NOT JOINED (%d, %d): the radio waits to be asked\n", LX_STEP, error,
		    state.last_error);
	}
}

static void
lx_thread(__unused void *parameter, __unused wait_result_t result)
{
	const struct hakios_qrtr_ops *transport = NULL;
	uint32_t wait_s = 180, i;
	int error;

	step = LX_STEP;
	say("wcn: %u: Linux's ath10k: waiting for the modem's QRTR (modem_step=10 modem_keep=1; wcn_wait=S, default 180)\n",
	    LX_STEP);
	PE_parse_boot_argn("wcn_wait", &wait_s, sizeof(wait_s));
	for (i = 0; i < wait_s * 10 && (transport = hakios_mpss_qrtr()) == NULL; i++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	if (transport == NULL) {
		say("wcn: %u: THE MODEM'S QRTR IS NOT THERE within %u s: ath10k is not started\n", LX_STEP, wait_s);
		goto out;
	}
	/* (step 7: without the WCSS's debug clock the WLAN PD dies once the board's data is in) */
	if (!option("wcn_qdss_off") && !hakios_mpss_aop("{class: clock, res: qdss, val: 1}")) {
		say("wcn: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE \"qdss on\": ath10k is not started\n", LX_STEP);
		goto out;
	}
	hakios_qmi_set_transport(transport);
	say("wcn: %u: the modem's QRTR is there after %u.%u s, the debug clock on: ath10k starts (supplies %s)\n", LX_STEP,
	    i / 10, i % 10, option("wcn_power") ? "allowed, wcn_power=1" : "NOT ALLOWED: it stops where it powers the radio");
	error = lx_start();
	say("wcn: %u: ath10k's start returns %d\n", LX_STEP, error);
	if (error == 0 && (option("wifi_join") || option("wifi_station"))) {
		say("wcn: %u: the driver stays up\n", LX_STEP);
		if (option("wifi_station") && !option("wifi_join")) {
			lx_join_given();
		}
		thread_terminate(current_thread());
		return;
	}
	lx_stop();
out:
	power_off();
	space_off();
	say("wcn: the steps are over\n");
	if (option("wcn_return")) {
		say("wcn: wcn_return=1: back to the loader\n");
		loader_return();
	}
	thread_terminate(current_thread());
}

/* ---- the steps ------------------------------------------------------------------------------------------ */

static bool (*const steps[STEPS])(void) = { step_smem, step_supplies, step_smmu, step_wlfw, step_msa_info, step_msa, step_board,
	                                     step_space, step_power, step_mission };
static uint32_t last;

/* Steps from `first` to `last`; then back to the loader with wcn_return=1. */
static void
run_steps(uint32_t first)
{
	for (step = first; step <= last; step++) {
		if (!steps[step - 1]()) {
			say("wcn: step %u did not find what it needs: the steps end here\n", step);
			break;
		}
	}
	if (last >= 4) {
		wlfw_close();           /* (the mode "off" first, while the radio is still powered) */
	}
	power_off();
	space_off();
	say("wcn: the steps are over\n");
	if (option("wcn_return")) {
		say("wcn: wcn_return=1: back to the loader\n");
		loader_return();
	}
}

static void
wlfw_thread(__unused void *parameter, __unused wait_result_t result)
{
	run_steps(4);
	thread_terminate(current_thread());
}

void
hakios_wcn3990_init(void)
{
	thread_t thread;

	bool lx = option("wcn_lx");

	if (lx) {
		last = 3;
	} else if (!PE_parse_boot_argn("wcn_step", &last, sizeof(last)) || last == 0) {
		return;
	}
	if (!is_cepheus()) {
		say("wcn: wcn_step and wcn_lx are for the Xiaomi Mi 9 (cepheus) only: nothing is done\n");
		return;
	}
	if (last > STEPS) {
		say("wcn: there are %u steps so far: wcn_step=%u is taken as %u\n", STEPS, last, STEPS);
		last = STEPS;
	}
	PE_parse_boot_argn("wcn_pause", &pause_ms, sizeof(pause_ms));
	say("wcn: the Wi-Fi of the Mi 9 (WCN3998 in the modem's WLAN PD): steps 1 to %u%s\n", last, lx ? ", then Linux's ath10k "
	    "(wcn_lx=1) on a thread of its own, beside the modem's" : last <= 3 ? ", reading only" : WLFW_BUILT ? "; from 4 on a "
	    "thread of their own, beside the modem's" : "");
	if (last <= 3 && !lx) {
		run_steps(1);
		return;
	}
	/* (1 to 3 here, before the modem is started: what the phone was left in) */
	for (step = 1; step <= 3; step++) {
		if (!steps[step - 1]()) {
			say("wcn: step %u did not find what it needs: the steps end here\n", step);
			return;
		}
	}
	/* (en0 now, before configd and wifid look for it: see hakios_wifi_radio_address()) */
	if (lx && option("wifi_station") && !option("wifi_join")) {
		hakios_lx_net_early();
	}
	if (kernel_thread_start(lx ? lx_thread : wlfw_thread, NULL, &thread) != KERN_SUCCESS) {
		say("wcn: no thread: steps 4 on are not performed\n");
		return;
	}
	thread_deallocate(thread);
}
