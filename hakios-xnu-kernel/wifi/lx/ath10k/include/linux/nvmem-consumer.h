/*
 * Cells of non-volatile memory, where some boards keep the radio's calibration (core.c:
 * ath10k_download_cal_nvmem()). This one has none: its calibration comes with the board's data
 * file. Every cell is missing.
 */
#ifndef _LX_ATH10K_LINUX_NVMEM_CONSUMER_H
#define _LX_ATH10K_LINUX_NVMEM_CONSUMER_H

#include <linux/err.h>
#include <linux/device.h>

struct nvmem_cell;

static inline struct nvmem_cell *
devm_nvmem_cell_get(struct device *dev, const char *name)
{
	return (ERR_PTR(-ENOENT));
}

static inline void *
nvmem_cell_read(struct nvmem_cell *cell, size_t *length)
{
	return (ERR_PTR(-ENOENT));
}

#endif
