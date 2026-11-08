/*
 * The platform ath10k's bus for the WCN3990 (snoc.c, built as it is) is probed on, on the Mi 9
 * (cepheus, SM8150, a WCN3998): the device tree's node of the radio as Linux has it for this SoC
 * (qcom,wcn3990-wifi), its registers and the interrupts of its twelve copy engines as platform
 * resources, and underneath what snoc.c and qmi.c call of the kernel -- supplies and clocks,
 * TrustZone's hand-over of the firmware's memory (MSA), the modem's restarts.
 *
 * The radio's firmware runs in the modem's WLAN PD, which the kernel has started before
 * lx_start() (drivers/wifi/hakios.wcn3990.c, steps 1 to 3: the modem offline, the PD up, WLFW
 * announced). From there the driver does what it does under Linux: qmi.c has its exchange with
 * the firmware up to "FW ready", snoc.c powers the radio and asks for mission mode, core.c brings
 * the copy engines, HTC, WMI and the data path up and registers the radio with what stands in for
 * mac80211 (../src/lx_80211.c).
 *
 * The copy engines' interrupts: snoc.c asks for one per engine (request_irq()); here no line of
 * the GIC is attached yet, and a thread calls the handler of every engine whose interrupt is
 * enabled once a millisecond, which is what an interrupt would make it do (the handler looks at
 * the engines' summary register and hands the work to NAPI). NAPI's poll runs on the same thread.
 *
 * What the kernel is asked here (bsd/sys/hakios.lx.h): lx_os_radio_supplies(), lx_os_radio_clocks()
 * and lx_os_radio_hand_over(); it refuses the first unless it was allowed to turn the radio's
 * supplies on (wcn_power=1), and the driver then stops where it would power the radio.
 */
#define LX_QMI_AS_IT_IS				/* (this file is where the driver's requests go past) */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/skbuff.h>

#include "core.h"
#include "debug.h"
#include "qmi.h"
#include "snoc.h"
#include "wmi.h"
#include "wmi-ops.h"

#include <lx_os.h>
#include "lx.h"

/* ---- the radio's node --------------------------------------------------------------------------- */

/*
 * As Linux's sm8150.dtsi has it (wifi@18800000), with the board's MSA region of the Mi 9's own
 * tree (qcom,wlan-msa-fixed-region of its qcom,icnss: 0x8bc00000, 1 MB; drivers/wifi/hakios.wcn3990.c).
 * The supplies and clocks are named there and turned on by the kernel; the interrupts are the
 * copy engines', GIC SPIs 414 to 425 (not attached: see above).
 */
#define RADIO_REGISTERS		0x18800000ull
#define RADIO_REGISTERS_SIZE	0x800000ul
#define MSA_PHYS		0x8bc00000ull
#define MSA_SIZE		0x100000ull
#define LINES			CE_COUNT_MAX
#define FIRST_LINE		1000		/* (a number of ours for an engine's interrupt: 1000 + the engine) */

static const char compatible[] = "qcom,wcn3990-wifi";
static const struct property radio_properties[] = {
	{ "compatible", sizeof(compatible), compatible },
};
static struct device_node radio_node = {
	.name = "wifi",
	.full_name = "wifi@18800000",
	.properties = radio_properties,
	.nproperties = nitems(radio_properties),
};

const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	for (int i = 0; np != NULL && i < np->nproperties; i++) {
		if (strcmp(np->properties[i].name, name) == 0) {
			if (lenp != NULL) {
				*lenp = np->properties[i].length;
			}
			return (&np->properties[i]);
		}
	}
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	const struct property *p = of_find_property(np, name, lenp);

	return (p != NULL ? p->value : NULL);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	int length;
	const char *value = of_get_property(np, name, &length);

	if (value == NULL) {
		return (-EINVAL);
	}
	if (length == 0 || strnlen(value, length) == (size_t)length) {
		return (-EILSEQ);
	}
	*out = value;
	return (0);
}

int
of_property_read_u32_index(const struct device_node *np, const char *name, u32 index, u32 *out)
{
	int length;
	const u32 *value = of_get_property(np, name, &length);

	if (value == NULL) {
		return (-EINVAL);
	}
	if ((size_t)length < (index + 1) * sizeof(u32)) {
		return (-EOVERFLOW);
	}
	*out = value[index];
	return (0);
}

bool
of_device_is_compatible(const struct device_node *np, const char *name)
{
	int length;
	const char *value = of_get_property(np, "compatible", &length), *end = value + length;

	for (; value != NULL && value < end; value += strnlen(value, end - value) + 1) {
		if (strcmp(value, name) == 0) {
			return (true);
		}
	}
	return (false);
}

bool
of_machine_is_compatible(const char *name)
{
	return (false);
}

/* (snoc.c asks for "wifi-firmware", the node of the firmware's own IOMMU domain: there is none) */
struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	return (NULL);
}

struct device_node *
of_find_node_by_phandle(phandle handle)
{
	return (NULL);
}

struct device_node *
of_parse_phandle(const struct device_node *np, const char *name, int index)
{
	return (NULL);
}

const struct of_device_id *
of_match_node(const struct of_device_id *matches, const struct device_node *np)
{
	for (; matches != NULL && matches->compatible[0] != 0; matches++) {
		if (of_device_is_compatible(np, matches->compatible)) {
			return (matches);
		}
	}
	return (NULL);
}

/* the radio's memory region: the MSA, which the firmware is told of (ath10k_setup_msa_resources()) */
int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	if (np != &radio_node || index != 0) {
		return (-ENODEV);
	}
	memset(res, 0, sizeof(*res));
	res->start = MSA_PHYS;
	res->end = MSA_PHYS + MSA_SIZE - 1;
	res->flags = IORESOURCE_MEM;
	return (0);
}

/* The radio's own address: the kernel's, if it was given one (mac.c asks before it takes the firmware's). */
int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}

/* ---- the platform device ------------------------------------------------------------------------ */

static struct resource radio_resources[] = {
	{ .start = RADIO_REGISTERS, .end = RADIO_REGISTERS + RADIO_REGISTERS_SIZE - 1, .name = "membase", .flags = IORESOURCE_MEM },
};
static struct platform_device radio = {
	.name = "ath10k_snoc",
	.id = -1,
	.num_resources = nitems(radio_resources),
	.resource = radio_resources,
};

struct resource *
platform_get_resource_byname(struct platform_device *pdev, unsigned int type, const char *name)
{
	for (u32 i = 0; i < pdev->num_resources; i++) {
		struct resource *r = &pdev->resource[i];

		if ((r->flags & type) != 0 && r->name != NULL && strcmp(r->name, name) == 0) {
			return (r);
		}
	}
	return (NULL);
}

int
platform_get_irq(struct platform_device *pdev, unsigned int index)
{
	return (pdev == &radio && index < LINES ? FIRST_LINE + (int)index : -ENXIO);
}

struct platform_device *
platform_device_register_full(const struct platform_device_info *info)
{
	return (ERR_PTR(-ENODEV));
}

void
platform_device_unregister(struct platform_device *pdev)
{
}

const void *
device_get_match_data(const struct device *dev)
{
	const struct of_device_id *id = of_match_node(lx_platform_driver->driver.of_match_table, dev->of_node);

	return (id != NULL ? id->data : NULL);
}

/*
 * The MSA, mapped as registers are (never cached, never read ahead): TrustZone hands it to the
 * modem, after which a read of ours would end the kernel. snoc.c reads it only for a dump after
 * the firmware has crashed.
 */
void *
devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags)
{
	void *mapped = lx_os_map_registers(offset, size);

	return (mapped != NULL ? mapped : ERR_PTR(-ENOMEM));
}

/* ---- supplies and clocks: the kernel's ---------------------------------------------------------- */

int
devm_regulator_bulk_get(struct device *dev, int count, struct regulator_bulk_data *supplies)
{
	static struct regulator one = { "the radio's supplies" };

	for (int i = 0; i < count; i++) {
		supplies[i].consumer = &one;
	}
	return (0);
}

/* (snoc.c turns them all on at once, and then its clocks: ath10k_hw_power_on()) */
int
regulator_bulk_enable(int count, struct regulator_bulk_data *supplies)
{
	int error = lx_os_radio_supplies(1);

	lx_tell("lx: the driver turns the radio's %d supplies on: %s (%d)\n", count, error == 0 ? "done" : "REFUSED", error);
	return (-error);
}

int
regulator_bulk_disable(int count, struct regulator_bulk_data *supplies)
{
	int error = lx_os_radio_supplies(0);

	lx_tell("lx: the driver turns the radio's supplies off (%d)\n", error);
	return (-error);
}

int
devm_clk_bulk_get_optional(struct device *dev, int count, struct clk_bulk_data *clocks)
{
	static struct clk one = { "the radio's clocks" };

	for (int i = 0; i < count; i++) {
		clocks[i].clk = &one;
	}
	return (0);
}

int
clk_bulk_prepare_enable(int count, const struct clk_bulk_data *clocks)
{
	int error = lx_os_radio_clocks(1);

	lx_tell("lx: the driver turns the radio's %d clocks on: %s (%d)\n", count, error == 0 ? "done" : "REFUSED", error);
	return (-error);
}

void
clk_bulk_disable_unprepare(int count, const struct clk_bulk_data *clocks)
{
	lx_os_radio_clocks(0);
}

/* ---- TrustZone, and the modem ------------------------------------------------------------------- */

/*
 * The MSA's hand-over, region by region as the firmware has said where they are
 * (ath10k_qmi_setup_msa_permissions()): from us to the modem and the WLAN (and the WLAN's copy
 * engines, where the region is not secure), and back when the driver goes. The kernel makes the
 * call to TrustZone (hakios.wcn3990.c, step 6, does the same) and refuses what is not inside the
 * MSA.
 */
int
qcom_scm_assign_mem(phys_addr_t address, size_t size, u64 *owners, const struct qcom_scm_vmperm *new_owners,
    unsigned int count)
{
	bool to_radio = false, ce = false;
	u64 mask = 0;
	int error;

	for (unsigned i = 0; i < count; i++) {
		to_radio |= new_owners[i].vmid != QCOM_SCM_VMID_HLOS;
		ce |= new_owners[i].vmid == QCOM_SCM_VMID_WLAN_CE;
		mask |= BIT_ULL(new_owners[i].vmid);
	}
	error = lx_os_radio_hand_over(address, size, to_radio, ce);
	lx_tell("lx: the driver has TrustZone hand %#llx (%zu bytes) to %s: %s (%d)\n", (unsigned long long)address, size,
	    to_radio ? "the modem and the WLAN" : "us", error == 0 ? "done" : "REFUSED", error);
	if (error == 0) {
		*owners = mask;
	}
	return (-error);
}

/* the modem's restarts: the kernel does not restart it, and the notifier is never called */
void *
qcom_register_ssr_notifier(const char *name, struct notifier_block *nb)
{
	static int cookie;

	return (&cookie);
}

int
qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb)
{
	return (0);
}

/* ---- the copy engines' interrupts, and NAPI ------------------------------------------------------- */

static struct {
	struct {
		irq_handler_t handler;
		void *argument;
		volatile bool enabled;
	} line[LINES];
	struct napi_struct *napi;		/* the driver's one (snoc.c: ar->napi) */
	volatile bool napi_on;
	volatile bool scheduled;		/* napi_schedule() since the poll last said it was done */
	volatile bool running;			/* the thread below is wanted ... */
	volatile bool runs;			/* ... and runs */
	volatile bool busy;			/* it is in a handler or the poll */
	void *self;				/* (the thread) */
	unsigned long long polls, calls;
} engines;

#define NAPI_BUDGET	64

/*
 * Who turns a line (or NAPI) off writes `enabled` and then reads `busy`; the thread writes `busy`
 * and then reads `enabled`. On several processors each may see the other's old value unless a
 * full barrier stands between the write and the read on both sides -- and then the thread would
 * call a handler, which writes the copy engine's registers, after the driver was told it no longer
 * would and has cut the radio's power (logs/174824: an SError in ath10k_ce_disable_interrupt()
 * from engines_look(), after a join that failed and the scans that powered the radio up and down).
 */
static inline void
engines_fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#if defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");	/* (and the device's writes done, not only ordered) */
#endif
}

static void
engines_look(void *argument)
{
	engines.self = lx_os_thread_self();
	engines.runs = true;
	while (engines.running) {
		bool more = false;

		engines.busy = true;
		engines_fence();		/* (before `enabled` is read: see engines_wait()) */
		for (unsigned i = 0; i < LINES; i++) {
			if (engines.line[i].enabled && engines.line[i].handler != NULL) {
				engines.calls++;
				engines.line[i].handler(FIRST_LINE + i, engines.line[i].argument);
			}
		}
		if (engines.scheduled && engines.napi_on && engines.napi != NULL) {
			engines.polls++;
			more = engines.napi->poll(engines.napi, NAPI_BUDGET) >= NAPI_BUDGET;
		}
		engines_fence();		/* (the registers written are written before the thread says it is out) */
		engines.busy = false;
		if (!more) {
			lx_pause_ms(1);
		}
	}
	engines.runs = false;
}

static void
engines_wait(void)
{
	engines_fence();
	if (lx_os_thread_self() == engines.self) {
		return;				/* (the thread itself: from a handler or the poll) */
	}
	while (engines.busy && engines.runs) {
		lx_pause_ms(1);
	}
}

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_handler,
    unsigned long flags, const char *name, void *argument)
{
	unsigned i = irq - FIRST_LINE;

	if (i >= LINES || engines.line[i].handler != NULL) {
		return (-EINVAL);
	}
	engines.line[i].argument = argument;
	engines.line[i].handler = handler;
	engines.line[i].enabled = (flags & IRQF_NO_AUTOEN) == 0;
	return (0);
}

int
lkpi_enable_irq(unsigned int irq)
{
	unsigned i = irq - FIRST_LINE;

	if (i < LINES) {
		engines.line[i].enabled = true;
	}
	return (0);
}

/* (as Linux's disable_irq(), it returns when the handler is not running: but not from the handler itself) */
void
lkpi_disable_irq(unsigned int irq)
{
	unsigned i = irq - FIRST_LINE;

	if (i < LINES) {
		engines.line[i].enabled = false;
		engines_wait();			/* (returns at once on the thread itself) */
	}
}

void
lkpi_free_irq(unsigned int irq, void *argument)
{
	unsigned i = irq - FIRST_LINE;

	if (i < LINES) {
		engines.line[i].enabled = false;
		engines_wait();
		engines.line[i].handler = NULL;
	}
}

void
lkpi_devm_free_irq(struct device *dev, unsigned int irq, void *argument)
{
	lkpi_free_irq(irq, argument);
}

/* snoc.c's NAPI device: nothing but a place for its NAPI (alloc_netdev_dummy()) */
struct net_device *
linuxkpi_alloc_netdev(size_t size, const char *name, uint32_t assign, void (*setup)(struct net_device *))
{
	struct net_device *dev = kzalloc(sizeof(*dev) + size, GFP_KERNEL);

	if (dev != NULL && setup != NULL) {
		setup(dev);
	}
	return (dev);
}

void
linuxkpi_free_netdev(struct net_device *dev)
{
	kfree(dev);
}

void
linuxkpi_netif_napi_add(struct net_device *dev, struct napi_struct *napi, int (*poll)(struct napi_struct *, int))
{
	napi->dev = dev;
	napi->poll = poll;
	napi->budget = NAPI_BUDGET;
	engines.napi = napi;
}

void
linuxkpi_netif_napi_del(struct napi_struct *napi)
{
	if (engines.napi == napi) {
		engines.napi_on = false;
		engines_wait();
		engines.napi = NULL;
	}
}

bool
linuxkpi_napi_schedule_prep(struct napi_struct *napi)
{
	return (engines.napi_on);
}

void
linuxkpi___napi_schedule(struct napi_struct *napi)
{
	engines.scheduled = true;
}

bool
linuxkpi_napi_schedule(struct napi_struct *napi)
{
	if (!engines.napi_on) {
		return (false);
	}
	engines.scheduled = true;
	return (true);
}

void
linuxkpi_napi_reschedule(struct napi_struct *napi)
{
	linuxkpi_napi_schedule(napi);
}

bool
linuxkpi_napi_complete_done(struct napi_struct *napi, int done)
{
	engines.scheduled = false;
	return (true);
}

bool
linuxkpi_napi_complete(struct napi_struct *napi)
{
	return (linuxkpi_napi_complete_done(napi, 0));
}

void
linuxkpi_napi_enable(struct napi_struct *napi)
{
	engines.napi_on = true;
}

void
linuxkpi_napi_disable(struct napi_struct *napi)
{
	engines.napi_on = false;
	engines_wait();
	engines.scheduled = false;
}

void
linuxkpi_napi_synchronize(struct napi_struct *napi)
{
	engines_wait();
}

/* ---- what the driver asks the firmware ----------------------------------------------------------- */

/*
 * One request matters here: the mode. The copy engines' registers answer in mission mode, and
 * snoc.c asks for it before it touches them and for "off" after it has stopped them
 * (ath10k_snoc_wlan_enable(), ..._disable()); this says when.
 */
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == QMI_WLFW_WLAN_MODE_REQ_V01) {
		lx_tell("lx: the firmware is asked for mode %u (0: mission, 4: off)\n",
		    ((const struct wlfw_wlan_mode_req_msg_v01 *)structure)->mode);
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- the entries ---------------------------------------------------------------------------------- */

static struct ath10k *volatile driver;	/* the driver, while it is up: between lx_start() and lx_stop() */

/*
 * The firmware's idle power save off, after every start of the radio (mac.c: ath10k_start() turns it on, idle_ps_config
 * 1). With it on, the firmware powers the radio's side of the copy engines down some 15 s after the radio has nothing to
 * do -- started, no network joined, no scan -- and wakes it for an interrupt. Linux waits for that interrupt; this layer
 * looks at the engines every millisecond (engines_look()), and its first write after that is an SError (logs/184108 and
 * 212706 of the kit's runs, both 15 s after "the radio is started" with no network given: ath10k_ce_disable_interrupt()
 * from engines_look()). Off for as long as the engines are polled rather than interrupted.
 */
static void
idle_power_save_off(void)
{
	struct ath10k *ar = driver;
	int ret;

	if (ar == NULL || ar->wmi.pdev_param == NULL) {
		lx_tell("lx: THE FIRMWARE'S IDLE POWER SAVE CANNOT BE TURNED OFF: no driver yet\n");
		return;
	}
	ret = ath10k_wmi_pdev_set_param(ar, ar->wmi.pdev_param->idle_ps_config, 0);
	lx_tell("lx: the firmware's idle power save (idle_ps_config) %s (%d): the copy engines stay powered while the radio idles\n",
	    ret == 0 ? "turned off" : "NOT TURNED OFF", ret);
}
static bool probed;
static void lx_stop_driver(void);

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

int
lx_start(void)
{
	struct ath10k *ar;
	unsigned long long began;
	bool up;
	int ret;

	if (lx_prepare() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	/*
	 * Not SNOC and HTC: once frames pass they say a line or two for every one of them
	 * (logs/165526: 13000 lines in five minutes), and the kernel's buffer keeps only those.
	 */
	if (!lx_os_option("wifi_quiet")) {
		ath10k_debug_mask = ATH10K_DBG_QMI | ATH10K_DBG_BOOT | ATH10K_DBG_WMI | ATH10K_DBG_MAC | ATH10K_DBG_REGULATORY;
	}
	engines.running = true;
	if (lx_os_thread("wifi-engines", engines_look, NULL) != 0) {
		engines.running = false;
		return (ENOMEM);
	}

	radio.dev.of_node = &radio_node;
	radio.dev.kobj.name = "wifi";
	/*
	 * LinuxKPI's dma_set_mask() wants a busdma tag here, and says -EIO without one; the radio's
	 * memory is the layer's pool (../src/lx_base.c), which reads nothing of it: anything but NULL
	 */
	radio.dev.dma_priv = &radio;
	lx_tell("lx: ath10k's bus (snoc.c) is probed on the WCN3990 at %#llx\n", RADIO_REGISTERS);
	if ((ret = lx_platform_driver->probe(&radio)) != 0) {
		lx_tell("lx: the probe fails: %d\n", ret);
		engines.running = false;
		return (-ret);
	}
	probed = true;
	ar = platform_get_drvdata(&radio);

	/*
	 * The rest happens on the driver's threads: qmi.c's exchange with the firmware once WLFW is
	 * there, then core.c's start of the firmware, and the radio's registration (its flag below).
	 */
	began = lx_os_nanoseconds();
	while (!test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags) && lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags);
	lx_tell("lx: %llu ms after the probe the driver %s; its flags %#lx, state %d; the engines' handlers called %llu times, "
	    "NAPI's poll %llu\n", (lx_os_nanoseconds() - began) / 1000000ull, up ? "IS UP: the radio is registered" :
	    "HAS NOT GOT THROUGH its start", ar->dev_flags, ar->state, engines.calls, engines.polls);
	if (!up) {
		lx_stop_driver();
		return (ETIMEDOUT);
	}

	driver = ar;
	lx_radio_started = idle_power_save_off;
	if (lx_os_option("wifi_station")) {
		if ((ret = lx_station_offer()) == 0) {
			return (0);
		}
	} else if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) == 0) {
			if (lx_os_option("wifi_hold")) {
				lx_pause_ms(1000 * lx_os_option("wifi_hold"));
				lx_station_say();
			}
			return (0);
		}
	} else {
		ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
	}
	lx_stop();
	return (ret);
}

/* the driver removed as Linux removes it (ath10k_snoc_remove(): the mode "off", the supplies off), and the thread stopped */
static void
lx_stop_driver(void)
{
	if (probed) {
		probed = false;
		lx_platform_driver->remove(&radio);
	}
	engines.running = false;
	while (engines.runs) {
		lx_pause_ms(1);
	}
}

void
lx_stop(void)
{
	if (driver == NULL) {
		return;
	}
	driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	lx_stop_driver();
	lx_tell("lx: the driver is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}
