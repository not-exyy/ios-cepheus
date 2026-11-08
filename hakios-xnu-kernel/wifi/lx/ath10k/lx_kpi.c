/*
 * What ath10k calls of FreeBSD's kernel and of LinuxKPI's own source files that ath11k does not:
 * waiting, the time, rate limits, CRC-32, devres, a hex dump, a buffer's copy, and of mac80211
 * the transmit queues. None of it is this driver's alone; it is here and not in ../src while
 * only this driver needs it (../src builds ath11k as it is).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include <lx_os.h>
#include "lx.h"

/* ---- waiting and the time ---------------------------------------------------------------------- */

/* udelay(), mdelay(): the processor waits, as Linux's do */
void
DELAY(int microseconds)
{
	unsigned long long until = lx_os_nanoseconds() + (unsigned long long)microseconds * 1000ull;

	while (lx_os_nanoseconds() < until) {
	}
}

/* (what msleep() and its kin come to: the thread sleeps) */
int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	unsigned long long ms = ((unsigned long long)sbt * 1000ull) >> 32;

	lx_pause_ms(ms != 0 ? (unsigned)ms : 1);
	return (EWOULDBLOCK);
}

void
nanouptime(struct timespec *tsp)
{
	unsigned long long now = lx_os_nanoseconds();

	tsp->tv_sec = now / 1000000000ull;
	tsp->tv_nsec = now % 1000000000ull;
}

/* at most `most` events a second (printk_ratelimited()): 1, it may be said; 0, not */
int
eventratecheck(struct timeval *last, int *count, int most)
{
	unsigned long long now = lx_os_nanoseconds() / 1000ull, then = (unsigned long long)last->tv_sec * 1000000ull + last->tv_usec;

	if (last->tv_sec == 0 || now - then >= 1000000ull) {
		last->tv_sec = now / 1000000ull;
		last->tv_usec = now % 1000000ull;
		*count = 0;
	}
	if (most < 0) {
		return (1);
	}
	return (*count < most && ++*count != 0);
}

/* ---- saying things ----------------------------------------------------------------------------- */

/*
 * dev_printk() and its kin, for a device FreeBSD would have: the line, after "ath10k: ". All of
 * the driver's own messages come here as "%pV" and a struct va_format (debug.c: ath10k_info(),
 * _warn(), _err(), _dbg()), which the kernel's printf does not know (it says the pointer and
 * a V): that one is taken apart here, the message inside it said as the driver wrote it.
 */
int
device_printf(device_t dev, const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	if (strcmp(format, "%pV") == 0) {
		struct va_format *vaf = va_arg(list, struct va_format *);
		va_list inner;

		va_copy(inner, *vaf->va);
		vsnprintf(line, sizeof(line), vaf->fmt, inner);
		va_end(inner);
	} else {
		vsnprintf(line, sizeof(line), format, list);
	}
	va_end(list);
	return (printf("ath10k: %s", line));
}

int
lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii)
{
	const uint8_t *bytes = buf;
	size_t at = 0;

	if (linebuflen == 0) {
		return (0);
	}
	linebuf[0] = 0;
	if (rowsize != 16 && rowsize != 32) {
		rowsize = 16;
	}
	for (size_t i = 0; i < len && i < (size_t)rowsize && at + 4 < linebuflen; i++) {
		at += snprintf(linebuf + at, linebuflen - at, "%s%02x", i == 0 ? "" : " ", bytes[i]);
	}
	return ((int)at);
}

size_t
strnlen(const char *s, size_t most)
{
	size_t n = 0;

	while (n < most && s[n] != 0) {
		n++;
	}
	return (n);
}

/* ---- CRC-32 (IEEE 802.3), for crc32_le(): FreeBSD's table, made by the compiler ----------------- */

#define CRC1(c)		(((c) >> 1) ^ (0xedb88320u & (0u - ((c) & 1u))))
#define CRC8(c)		CRC1(CRC1(CRC1(CRC1(CRC1(CRC1(CRC1(CRC1((uint32_t)(c)))))))))
#define CRC_4(i)	CRC8(i), CRC8((i) + 1), CRC8((i) + 2), CRC8((i) + 3)
#define CRC_16(i)	CRC_4(i), CRC_4((i) + 4), CRC_4((i) + 8), CRC_4((i) + 12)
#define CRC_64(i)	CRC_16(i), CRC_16((i) + 16), CRC_16((i) + 32), CRC_16((i) + 48)
const uint32_t crc32_tab[256] = { CRC_64(0u), CRC_64(64u), CRC_64(128u), CRC_64(192u) };

/* ---- memory a device holds (devm_*()) ---------------------------------------------------------- */

/*
 * Linux gives it back when the driver lets go of the device; here the driver is not unloaded
 * and the device not let go of, and it stays taken (a second probe takes more).
 */
void *
lkpi_devres_alloc(void (*release)(struct device *, void *), size_t size, gfp_t gfp)
{
	return (kzalloc(size, gfp));
}

void
lkpi_devres_add(struct device *dev, void *p)
{
}

void
lkpi_devm_kmalloc_release(struct device *dev, void *p)
{
}

/* ---- memory the radio reads and writes ---------------------------------------------------------- */

int
linux_dma_tag_init(struct device *dev, u64 mask)
{
	return (0);			/* (all of the pool is below any mask: it is the radio's own view, see ../src/lx_base.c) */
}

int
linux_dma_tag_init_coherent(struct device *dev, u64 mask)
{
	return (0);
}

/* (never given back, like the rest of the pool's rings) */
void *
linuxkpi_dmam_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
{
	return (linux_dma_alloc_coherent(dev, size, dma_handle, flag));
}

/* a buffer and its data, copied into a new one of the pool (htt_rx.c, for frames it hands on twice) */
struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	size_t headroom = skb_headroom(skb);
	struct sk_buff *copy;

	if (headroom >= NET_SKB_PAD) {
		headroom -= NET_SKB_PAD;		/* (dev_alloc_skb() reserves that much itself) */
	}
	copy = linuxkpi_dev_alloc_skb(headroom + skb->len, gfp);
	if (copy == NULL) {
		return (NULL);
	}
	skb_reserve(copy, headroom);
	memcpy(skb_put(copy, skb->len), skb->data, skb->len);
	memcpy(copy->cb, skb->cb, sizeof(copy->cb));
	copy->priority = skb->priority;
	copy->protocol = skb->protocol;
	return (copy);
}

/* ---- mac80211 and cfg80211 --------------------------------------------------------------------- */

const uint8_t rfc1042_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

uint8_t
linuxkpi_ieee80211_get_tid(struct ieee80211_hdr *hdr, bool nonqos_ok)
{
	if (!ieee80211_is_data_qos(hdr->frame_control)) {
		return (nonqos_ok ? IEEE80211_NONQOS_TID : 0);
	}
	return (ieee80211_get_qos_ctl(hdr)[0] & IEEE80211_QOS_CTL_TID_MASK);
}

/*
 * The transmit queues: what stands in for mac80211 hands every frame to the driver's tx() as it
 * comes (../src/lx_80211.c), and keeps none in queues; the driver's questions about them find
 * them empty. Stopping a queue is said and has no other effect: the station sends one frame at a
 * time in any case.
 */
void
linuxkpi_ieee80211_txq_schedule_start(struct ieee80211_hw *hw, uint8_t ac)
{
}

struct ieee80211_txq *
linuxkpi_ieee80211_next_txq(struct ieee80211_hw *hw, uint8_t ac)
{
	return (NULL);
}

void
linuxkpi_ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq, bool withoutpkts)
{
}

struct sk_buff *
linuxkpi_ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	return (NULL);
}

void
linuxkpi_ieee80211_txq_get_depth(struct ieee80211_txq *txq, unsigned long *frames, unsigned long *bytes)
{
	if (frames != NULL) {
		*frames = 0;
	}
	if (bytes != NULL) {
		*bytes = 0;
	}
}

void
linuxkpi_ieee80211_stop_queue(struct ieee80211_hw *hw, int queue)
{
	printf("lx: the driver stops its transmit queue %d\n", queue);
}

void
linuxkpi_ieee80211_wake_queue(struct ieee80211_hw *hw, int queue)
{
}

struct linuxkpi_ieee80211_regdomain *
lkpi_get_linuxkpi_ieee80211_regdomain(size_t rules)
{
	struct linuxkpi_ieee80211_regdomain *regd;

	regd = kzalloc(sizeof(*regd) + rules * sizeof(struct ieee80211_reg_rule), GFP_KERNEL);
	if (regd != NULL) {
		regd->n_reg_rules = (int)rules;
	}
	return (regd);
}

/*
 * A rate in units of 100 kbit/s, as cfg80211 reckons it (net/wireless/util.c): HT from its MCS,
 * VHT from its MCS and streams, both with the width and guard interval; legacy rates as they are.
 */
static const uint16_t vht_20[10] = { 65, 130, 195, 260, 390, 520, 585, 650, 780, 867 };	/* (x10, one stream, long GI) */

uint32_t
linuxkpi_cfg80211_calculate_bitrate(struct rate_info *rate)
{
	uint64_t r;
	unsigned streams;

	if ((rate->flags & (RATE_INFO_FLAGS_MCS | RATE_INFO_FLAGS_VHT_MCS)) == 0) {
		return (rate->legacy);
	}
	if (rate->flags & RATE_INFO_FLAGS_MCS) {
		streams = rate->mcs / 8 + 1;
		r = vht_20[rate->mcs % 8 < 8 ? rate->mcs % 8 : 0];
	} else {
		if (rate->mcs > 9) {
			return (0);
		}
		streams = rate->nss != 0 ? rate->nss : 1;
		r = vht_20[rate->mcs];
	}
	r *= streams;
	switch (rate->bw) {
	case RATE_INFO_BW_40:
		r = r * 27 / 13;		/* (108 data subcarriers for 52) */
		break;
	case RATE_INFO_BW_80:
		r = r * 117 / 26;		/* (234 for 52) */
		break;
	case RATE_INFO_BW_160:
		r = r * 117 / 13;
		break;
	default:
		break;
	}
	if (rate->flags & RATE_INFO_FLAGS_SHORT_GI) {
		r = r * 10 / 9;
	}
	return ((uint32_t)r);
}
