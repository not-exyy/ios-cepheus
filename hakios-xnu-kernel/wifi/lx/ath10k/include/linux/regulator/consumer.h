/*
 * Supplies, as snoc.c takes them: by name, all at once (devm_regulator_bulk_get()). What turns
 * them on and off is the kernel's (lx_snoc.c).
 */
#ifndef _LX_ATH10K_LINUX_REGULATOR_CONSUMER_H
#define _LX_ATH10K_LINUX_REGULATOR_CONSUMER_H

#include_next <linux/regulator/consumer.h>
#include <linux/device.h>

struct regulator {
	const char *name;
};

struct regulator_bulk_data {
	const char *supply;
	struct regulator *consumer;
	int init_load_uA;
	int ret;
};

int devm_regulator_bulk_get(struct device *dev, int count, struct regulator_bulk_data *supplies);
int regulator_bulk_enable(int count, struct regulator_bulk_data *supplies);
int regulator_bulk_disable(int count, struct regulator_bulk_data *supplies);

#endif
