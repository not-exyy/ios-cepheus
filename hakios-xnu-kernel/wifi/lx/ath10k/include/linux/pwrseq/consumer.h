/*
 * Power sequencers, which newer device trees put between a WCN399x and its supplies. There are
 * none here, and snoc.c is told so as Linux tells it when a tree names the supplies itself:
 * -EPROBE_DEFER, after which it takes them one by one, and its sequencer stays NULL: turning that
 * on or off is nothing and succeeds, as Linux's pwrseq_power_on(NULL) does.
 */
#ifndef _LX_ATH10K_LINUX_PWRSEQ_CONSUMER_H
#define _LX_ATH10K_LINUX_PWRSEQ_CONSUMER_H

#include <linux/err.h>
#include <linux/device.h>

struct pwrseq_desc;

static inline struct pwrseq_desc *
devm_pwrseq_get(struct device *dev, const char *target)
{
	return (ERR_PTR(-EPROBE_DEFER));
}

static inline int
pwrseq_power_on(struct pwrseq_desc *desc)
{
	return (0);
}

static inline int
pwrseq_power_off(struct pwrseq_desc *desc)
{
	return (0);
}

#endif
