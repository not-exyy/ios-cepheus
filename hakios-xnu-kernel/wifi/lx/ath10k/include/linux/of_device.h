/* Devices of the device tree: the radio's own (lx_snoc.c). */
#ifndef _LX_ATH10K_LINUX_OF_DEVICE_H
#define _LX_ATH10K_LINUX_OF_DEVICE_H

#include <linux/device.h>
#include <linux/of.h>

/* (for a device of the firmware's memory: there is none, see linux/platform_device.h) */
static inline int
of_dma_configure(struct device *dev, struct device_node *np, bool force)
{
	return (-ENODEV);
}

#endif
