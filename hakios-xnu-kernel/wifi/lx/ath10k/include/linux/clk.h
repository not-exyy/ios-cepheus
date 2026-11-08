/*
 * Clocks, as snoc.c takes them: by name, all at once (devm_clk_bulk_get_optional()). The radio's
 * two are turned on with its supplies (lx_snoc.c: the kernel's power for the radio).
 */
#ifndef _LX_ATH10K_LINUX_CLK_H
#define _LX_ATH10K_LINUX_CLK_H

#include <linux/device.h>

struct clk {
	const char *name;
};

struct clk_bulk_data {
	const char *id;
	struct clk *clk;
};

int devm_clk_bulk_get_optional(struct device *dev, int count, struct clk_bulk_data *clocks);
int clk_bulk_prepare_enable(int count, const struct clk_bulk_data *clocks);
void clk_bulk_disable_unprepare(int count, const struct clk_bulk_data *clocks);

#endif
