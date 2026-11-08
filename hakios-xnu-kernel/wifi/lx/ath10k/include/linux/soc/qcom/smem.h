/*
 * Shared memory (SMEM), where qmi.c writes the version of the WLAN's firmware into the table of
 * images' versions (ath10k_qmi_add_wlan_ver_smem()) for whoever reads it later; nobody does here.
 * The item is not found, which qmi.c says and goes on.
 */
#ifndef _LX_ATH10K_LINUX_SOC_QCOM_SMEM_H
#define _LX_ATH10K_LINUX_SOC_QCOM_SMEM_H

#include <linux/err.h>

#define QCOM_SMEM_HOST_ANY	-1

static inline void *
qcom_smem_get(unsigned host, unsigned item, size_t *size)
{
	return (ERR_PTR(-ENOENT));
}

#endif
