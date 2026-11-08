/*
 * The call of qcom_scm (TrustZone) that qmi.c makes: memory handed from one owner to others, the
 * MSA region from us (HLOS) to the modem and the WLAN when the firmware has said where it is
 * (ath10k_qmi_map_msa_permission()). The kernel does it (lx_snoc.c passes it on).
 */
#ifndef _LX_ATH10K_LINUX_QCOM_SCM_H
#define _LX_ATH10K_LINUX_QCOM_SCM_H

#include <linux/types.h>

#define QCOM_SCM_VMID_HLOS	0x3
#define QCOM_SCM_VMID_MSS_MSA	0xf
#define QCOM_SCM_VMID_WLAN	0x18
#define QCOM_SCM_VMID_WLAN_CE	0x19

#define QCOM_SCM_PERM_READ	0x4
#define QCOM_SCM_PERM_WRITE	0x2
#define QCOM_SCM_PERM_EXEC	0x1
#define QCOM_SCM_PERM_RW	(QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE)
#define QCOM_SCM_PERM_RWX	(QCOM_SCM_PERM_RW | QCOM_SCM_PERM_EXEC)

struct qcom_scm_vmperm {
	int vmid;
	int perm;
};

int qcom_scm_assign_mem(phys_addr_t address, size_t size, u64 *owners, const struct qcom_scm_vmperm *new_owners,
    unsigned int count);

#endif
