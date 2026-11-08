/* Properties of a device, which here are those of its node. */
#ifndef _LX_ATH10K_LINUX_PROPERTY_H
#define _LX_ATH10K_LINUX_PROPERTY_H

#include <linux/device.h>
#include <linux/of.h>

static inline int
device_property_read_u32(const struct device *dev, const char *name, u32 *out)
{
	return (dev->of_node != NULL ? of_property_read_u32(dev->of_node, name, out) : -EINVAL);
}

/* the data of the entry of the driver's table that matched the device (lx_snoc.c) */
const void *device_get_match_data(const struct device *dev);

#endif
