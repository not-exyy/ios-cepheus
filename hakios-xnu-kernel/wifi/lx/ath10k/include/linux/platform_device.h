/*
 * Platform devices, as snoc.c is written for them: a driver probed on a device of the device
 * tree, with its registers and interrupts as resources. There is one driver here, and one
 * device: lx_snoc.c makes the device and probes the driver on it; module_platform_driver() only
 * says which driver that is.
 */
#ifndef _LX_ATH10K_LINUX_PLATFORM_DEVICE_H
#define _LX_ATH10K_LINUX_PLATFORM_DEVICE_H

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/of.h>

struct platform_device {
	const char *name;
	int id;
	struct device dev;
	u32 num_resources;
	struct resource *resource;
	const int *irqs;			/* the interrupts, in the device tree's order */
	unsigned num_irqs;
};

struct platform_driver {
	int (*probe)(struct platform_device *);
	void (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
};

struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	bool of_node_reused;
	const char *name;
	int id;
	const struct resource *res;
	unsigned int num_res;
	const void *data;
	size_t size_data;
	u64 dma_mask;
};

#define to_platform_device(d)	container_of((d), struct platform_device, dev)
#define dev_is_platform(d)	(true)

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return (dev_get_drvdata(&pdev->dev));
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

struct resource *platform_get_resource_byname(struct platform_device *pdev, unsigned int type, const char *name);
int platform_get_irq(struct platform_device *pdev, unsigned int index);
/* (snoc.c makes one for the firmware's memory where the tree has a node "wifi-firmware"; this one has not) */
struct platform_device *platform_device_register_full(const struct platform_device_info *info);
void platform_device_unregister(struct platform_device *pdev);

/* the driver, which lx_snoc.c probes */
extern struct platform_driver *lx_platform_driver;
#define module_platform_driver(d)	struct platform_driver *lx_platform_driver = &(d)

#endif
