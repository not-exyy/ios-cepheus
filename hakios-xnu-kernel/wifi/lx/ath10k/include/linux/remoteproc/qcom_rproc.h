/*
 * What becomes of the modem (subsystem restart, SSR), which snoc.c wants to hear of: the WLAN's
 * firmware runs in it. The kernel does not restart the modem here, and says nothing: the
 * notifier is taken and never called.
 */
#ifndef _LX_ATH10K_LINUX_REMOTEPROC_QCOM_RPROC_H
#define _LX_ATH10K_LINUX_REMOTEPROC_QCOM_RPROC_H

#include <linux/notifier.h>

enum qcom_ssr_notify_type {
	QCOM_SSR_BEFORE_POWERUP,
	QCOM_SSR_AFTER_POWERUP,
	QCOM_SSR_BEFORE_SHUTDOWN,
	QCOM_SSR_AFTER_SHUTDOWN,
};

struct qcom_ssr_notify_data {
	const char *name;
	bool crashed;
};

void *qcom_register_ssr_notifier(const char *name, struct notifier_block *nb);
int qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb);

#endif
