/*
 * Included before every file of ath10k, after the layer's own prelude (../../include/lx_prelude.h):
 * what Linux's headers give its files in passing and FreeBSD's LinuxKPI does not.
 */
#ifndef _LX_ATH10K_H_
#define _LX_ATH10K_H_

#include <linux/delay.h>		/* (htc.c and hw.c wait without including it) */
#include <linux/compiler.h>
/*
 * The compiler here knows the attribute, but not typeof() of an array that has it, which
 * kzalloc_flex() takes (ce.c): the bound is a check and nothing else, and is dropped.
 */
#undef __counted_by
#define __counted_by(_x)
#include <linux/spinlock.h>
#include <linux/cleanup.h>
DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))
#include <linux/of.h>
#include <linux/io.h>

/*
 * The MSA region, which snoc.c maps (ath10k_setup_msa_resources(); it reads it only for a dump
 * after a crash): mapped as the radio's registers are, never cached nor read ahead -- once
 * TrustZone has handed it to the modem, a read of ours ends the kernel (lx_snoc.c).
 */
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags);

static inline int
of_property_read_u8_array(const struct device_node *np, const char *name, u8 *out, size_t count)
{
	int length;
	const u8 *value = of_get_property(np, name, &length);

	if (value == NULL) {
		return (-EINVAL);
	}
	if ((size_t)length < count) {
		return (-EOVERFLOW);
	}
	memcpy(out, value, count);
	return (0);
}

/* the index-th of the strings of a property (mac.c asks for "ext-fem-name") */
static inline int
of_property_read_string_index(const struct device_node *np, const char *name, int index, const char **out)
{
	int length;
	const char *value = of_get_property(np, name, &length), *end = value + length;

	if (value == NULL) {
		return (-EINVAL);
	}
	for (; index > 0 && value < end; index--) {
		value += strnlen(value, end - value) + 1;
	}
	if (value >= end) {
		return (-ENODATA);
	}
	*out = value;
	return (0);
}

static inline int
of_property_read_u8(const struct device_node *np, const char *name, u8 *out)
{
	return (of_property_read_u8_array(np, name, out, 1));
}

#include <linux/interrupt.h>
#include <linux/netdevice.h>

/* an interrupt that is not enabled by request_irq() (snoc.c enables its own when the copy engines are up) */
#define IRQF_NO_AUTOEN		0x00080000
/* (the copy engines' NAPI runs on the layer's threads in any case) */
static inline void netif_threaded_enable(struct net_device *dev) { }

#endif
