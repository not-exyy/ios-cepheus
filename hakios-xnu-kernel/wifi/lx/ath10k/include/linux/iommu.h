/*
 * IOMMU domains, which snoc.c makes for the firmware's memory where the device tree has a node
 * "wifi-firmware" (ath10k_fw_init()). This one has not: what the radio sees of memory is the
 * kernel's concern (the SMMU's stream of the radio, drivers/wifi/hakios.wcn3990.c). The calls are
 * there for the code to compile, and fail.
 */
#ifndef _LX_ATH10K_LINUX_IOMMU_H
#define _LX_ATH10K_LINUX_IOMMU_H

#include_next <linux/iommu.h>
#include <linux/err.h>

#define IOMMU_READ	(1 << 0)
#define IOMMU_WRITE	(1 << 1)

static inline struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev)
{
	return (ERR_PTR(-ENODEV));
}

static inline void iommu_domain_free(struct iommu_domain *domain) { }

static inline int
iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	return (-ENODEV);
}

static inline void iommu_detach_device(struct iommu_domain *domain, struct device *dev) { }

static inline int
iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	return (-ENODEV);
}

static inline size_t
iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	return (0);
}

#endif
