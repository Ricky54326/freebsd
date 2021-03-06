/*-
 * Copyright (C) 2013 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>
#include <machine/pmap.h>
#include <machine/resource.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "ntb_regs.h"
#include "ntb_hw.h"

/*
 * The Non-Transparent Bridge (NTB) is a device on some Intel processors that
 * allows you to connect two systems using a PCI-e link.
 *
 * This module contains the hardware abstraction layer for the NTB. It allows
 * you to send and recieve interrupts, map the memory windows and send and
 * receive messages in the scratch-pad registers.
 *
 * NOTE: Much of the code in this module is shared with Linux. Any patches may
 * be picked up and redistributed in Linux with a dual GPL/BSD license.
 */

#define NTB_CONFIG_BAR	0
#define NTB_B2B_BAR_1	1
#define NTB_B2B_BAR_2	2
#define NTB_B2B_BAR_3	3
#define NTB_MAX_BARS	4
#define NTB_MW_TO_BAR(mw) ((mw) + 1)

#define MAX_MSIX_INTERRUPTS MAX(XEON_MAX_DB_BITS, SOC_MAX_DB_BITS)

#define NTB_HB_TIMEOUT	1 /* second */
#define SOC_LINK_RECOVERY_TIME	500

#define DEVICE2SOFTC(dev) ((struct ntb_softc *) device_get_softc(dev))

enum ntb_device_type {
	NTB_XEON,
	NTB_SOC
};

/* Device features and workarounds */
#define HAS_FEATURE(feature)	\
	((ntb->features & (feature)) != 0)

struct ntb_hw_info {
	uint32_t		device_id;
	const char		*desc;
	enum ntb_device_type	type;
	uint32_t		features;
};

struct ntb_pci_bar_info {
	bus_space_tag_t		pci_bus_tag;
	bus_space_handle_t	pci_bus_handle;
	int			pci_resource_id;
	struct resource		*pci_resource;
	vm_paddr_t		pbase;
	void			*vbase;
	u_long			size;
};

struct ntb_int_info {
	struct resource	*res;
	int		rid;
	void		*tag;
};

struct ntb_db_cb {
	ntb_db_callback		callback;
	unsigned int		db_num;
	void			*data;
	struct ntb_softc	*ntb;
	struct callout		irq_work;
	bool			reserved;
};

struct ntb_softc {
	device_t		device;
	enum ntb_device_type	type;
	uint64_t		features;

	struct ntb_pci_bar_info	bar_info[NTB_MAX_BARS];
	struct ntb_int_info	int_info[MAX_MSIX_INTERRUPTS];
	uint32_t		allocated_interrupts;

	struct callout		heartbeat_timer;
	struct callout		lr_timer;

	void			*ntb_transport;
	ntb_event_callback	event_cb;
	struct ntb_db_cb	*db_cb;
	uint8_t			max_cbs;

	struct {
		uint8_t max_mw;
		uint8_t max_spads;
		uint8_t max_db_bits;
		uint8_t msix_cnt;
	} limits;
	struct {
		uint32_t ldb;
		uint32_t ldb_mask;
		uint32_t rdb;
		uint32_t bar2_xlat;
		uint32_t bar4_xlat;
		uint32_t bar5_xlat;
		uint32_t spad_remote;
		uint32_t spad_local;
		uint32_t lnk_cntl;
		uint32_t lnk_stat;
		uint32_t spci_cmd;
	} reg_ofs;
	uint32_t ppd;
	uint8_t conn_type;
	uint8_t dev_type;
	uint8_t bits_per_vector;
	uint8_t link_status;
	uint8_t link_width;
	uint8_t link_speed;
};

#ifdef __i386__
static __inline uint64_t
bus_space_read_8(bus_space_tag_t tag, bus_space_handle_t handle,
    bus_size_t offset)
{

	return (bus_space_read_4(tag, handle, offset) |
	    ((uint64_t)bus_space_read_4(tag, handle, offset + 4)) << 32);
}

static __inline void
bus_space_write_8(bus_space_tag_t tag, bus_space_handle_t handle,
    bus_size_t offset, uint64_t val)
{

	bus_space_write_4(tag, handle, offset, val);
	bus_space_write_4(tag, handle, offset + 4, val >> 32);
}
#endif

#define ntb_bar_read(SIZE, bar, offset) \
	    bus_space_read_ ## SIZE (ntb->bar_info[(bar)].pci_bus_tag, \
	    ntb->bar_info[(bar)].pci_bus_handle, (offset))
#define ntb_bar_write(SIZE, bar, offset, val) \
	    bus_space_write_ ## SIZE (ntb->bar_info[(bar)].pci_bus_tag, \
	    ntb->bar_info[(bar)].pci_bus_handle, (offset), (val))
#define ntb_reg_read(SIZE, offset) ntb_bar_read(SIZE, NTB_CONFIG_BAR, offset)
#define ntb_reg_write(SIZE, offset, val) \
	    ntb_bar_write(SIZE, NTB_CONFIG_BAR, offset, val)
#define ntb_mw_read(SIZE, offset) \
	    ntb_bar_read(SIZE, NTB_MW_TO_BAR(ntb->limits.max_mw), offset)
#define ntb_mw_write(SIZE, offset, val) \
	    ntb_bar_write(SIZE, NTB_MW_TO_BAR(ntb->limits.max_mw), \
		offset, val)

typedef int (*bar_map_strategy)(struct ntb_softc *ntb,
    struct ntb_pci_bar_info *bar);

static int ntb_probe(device_t device);
static int ntb_attach(device_t device);
static int ntb_detach(device_t device);
static int ntb_map_pci_bars(struct ntb_softc *ntb);
static int map_pci_bar(struct ntb_softc *ntb, bar_map_strategy strategy,
    struct ntb_pci_bar_info *bar);
static int map_mmr_bar(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar);
static int map_memory_window_bar(struct ntb_softc *ntb,
    struct ntb_pci_bar_info *bar);
static void ntb_unmap_pci_bar(struct ntb_softc *ntb);
static int ntb_remap_msix(device_t, uint32_t desired, uint32_t avail);
static int ntb_setup_interrupts(struct ntb_softc *ntb);
static int ntb_setup_legacy_interrupt(struct ntb_softc *ntb);
static int ntb_setup_xeon_msix(struct ntb_softc *ntb, uint32_t num_vectors);
static int ntb_setup_soc_msix(struct ntb_softc *ntb, uint32_t num_vectors);
static void ntb_teardown_interrupts(struct ntb_softc *ntb);
static void handle_soc_irq(void *arg);
static void handle_xeon_irq(void *arg);
static void handle_xeon_event_irq(void *arg);
static void ntb_handle_legacy_interrupt(void *arg);
static void ntb_irq_work(void *arg);
static uint64_t db_ioread(struct ntb_softc *, uint32_t regoff);
static void db_iowrite(struct ntb_softc *, uint32_t regoff, uint64_t val);
static void mask_ldb_interrupt(struct ntb_softc *ntb, unsigned int idx);
static void unmask_ldb_interrupt(struct ntb_softc *ntb, unsigned int idx);
static int ntb_create_callbacks(struct ntb_softc *ntb, uint32_t num_vectors);
static void ntb_free_callbacks(struct ntb_softc *ntb);
static struct ntb_hw_info *ntb_get_device_info(uint32_t device_id);
static void ntb_detect_max_mw(struct ntb_softc *ntb);
static int ntb_detect_xeon(struct ntb_softc *ntb);
static int ntb_detect_soc(struct ntb_softc *ntb);
static int ntb_setup_xeon(struct ntb_softc *ntb);
static int ntb_setup_soc(struct ntb_softc *ntb);
static void ntb_teardown_xeon(struct ntb_softc *ntb);
static void configure_soc_secondary_side_bars(struct ntb_softc *ntb);
static void configure_xeon_secondary_side_bars(struct ntb_softc *ntb);
static void ntb_handle_heartbeat(void *arg);
static void ntb_handle_link_event(struct ntb_softc *ntb, int link_state);
static void ntb_hw_link_down(struct ntb_softc *ntb);
static void ntb_hw_link_up(struct ntb_softc *ntb);
static void recover_soc_link(void *arg);
static int ntb_check_link_status(struct ntb_softc *ntb);
static void save_bar_parameters(struct ntb_pci_bar_info *bar);

static struct ntb_hw_info pci_ids[] = {
	{ 0x0C4E8086, "Atom Processor S1200 NTB Primary B2B", NTB_SOC, 0 },

	/* XXX: PS/SS IDs left out until they are supported. */
	{ 0x37258086, "JSF Xeon C35xx/C55xx Non-Transparent Bridge B2B",
		NTB_XEON, NTB_REGS_THRU_MW | NTB_B2BDOORBELL_BIT14 },
	{ 0x3C0D8086, "SNB Xeon E5/Core i7 Non-Transparent Bridge B2B",
		NTB_XEON, NTB_REGS_THRU_MW | NTB_B2BDOORBELL_BIT14 },
	{ 0x0E0D8086, "IVT Xeon E5 V2 Non-Transparent Bridge B2B", NTB_XEON,
		NTB_REGS_THRU_MW | NTB_B2BDOORBELL_BIT14 | NTB_SB01BASE_LOCKUP
		    | NTB_BAR_SIZE_4K },
	{ 0x2F0D8086, "HSX Xeon E5 V3 Non-Transparent Bridge B2B", NTB_XEON,
		NTB_REGS_THRU_MW | NTB_B2BDOORBELL_BIT14 | NTB_SB01BASE_LOCKUP
	},
	{ 0x6F0D8086, "BDX Xeon E5 V4 Non-Transparent Bridge B2B", NTB_XEON,
		NTB_REGS_THRU_MW | NTB_B2BDOORBELL_BIT14 | NTB_SB01BASE_LOCKUP
	},

	{ 0x00000000, NULL, NTB_SOC, 0 }
};

/*
 * OS <-> Driver interface structures
 */
MALLOC_DEFINE(M_NTB, "ntb_hw", "ntb_hw driver memory allocations");

static device_method_t ntb_pci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,     ntb_probe),
	DEVMETHOD(device_attach,    ntb_attach),
	DEVMETHOD(device_detach,    ntb_detach),
	DEVMETHOD_END
};

static driver_t ntb_pci_driver = {
	"ntb_hw",
	ntb_pci_methods,
	sizeof(struct ntb_softc),
};

static devclass_t ntb_devclass;
DRIVER_MODULE(ntb_hw, pci, ntb_pci_driver, ntb_devclass, NULL, NULL);
MODULE_VERSION(ntb_hw, 1);

SYSCTL_NODE(_hw, OID_AUTO, ntb, CTLFLAG_RW, 0, "NTB sysctls");

/*
 * OS <-> Driver linkage functions
 */
static int
ntb_probe(device_t device)
{
	struct ntb_hw_info *p;

	p = ntb_get_device_info(pci_get_devid(device));
	if (p == NULL)
		return (ENXIO);

	device_set_desc(device, p->desc);
	return (0);
}

static int
ntb_attach(device_t device)
{
	struct ntb_softc *ntb;
	struct ntb_hw_info *p;
	int error;

	ntb = DEVICE2SOFTC(device);
	p = ntb_get_device_info(pci_get_devid(device));

	ntb->device = device;
	ntb->type = p->type;
	ntb->features = p->features;

	/* Heartbeat timer for NTB_SOC since there is no link interrupt */
	callout_init(&ntb->heartbeat_timer, 1);
	callout_init(&ntb->lr_timer, 1);

	if (ntb->type == NTB_SOC)
		error = ntb_detect_soc(ntb);
	else
		error = ntb_detect_xeon(ntb);
	if (error)
		goto out;

	ntb_detect_max_mw(ntb);

	error = ntb_map_pci_bars(ntb);
	if (error)
		goto out;
	if (ntb->type == NTB_SOC)
		error = ntb_setup_soc(ntb);
	else
		error = ntb_setup_xeon(ntb);
	if (error)
		goto out;
	error = ntb_setup_interrupts(ntb);
	if (error)
		goto out;

	pci_enable_busmaster(ntb->device);

out:
	if (error != 0)
		ntb_detach(device);
	return (error);
}

static int
ntb_detach(device_t device)
{
	struct ntb_softc *ntb;

	ntb = DEVICE2SOFTC(device);
	callout_drain(&ntb->heartbeat_timer);
	callout_drain(&ntb->lr_timer);
	if (ntb->type == NTB_XEON)
		ntb_teardown_xeon(ntb);
	ntb_teardown_interrupts(ntb);

	/*
	 * Redetect total MWs so we unmap properly -- in case we lowered the
	 * maximum to work around Xeon errata.
	 */
	ntb_detect_max_mw(ntb);
	ntb_unmap_pci_bar(ntb);

	return (0);
}

static int
ntb_map_pci_bars(struct ntb_softc *ntb)
{
	int rc;

	ntb->bar_info[NTB_CONFIG_BAR].pci_resource_id = PCIR_BAR(0);
	rc = map_pci_bar(ntb, map_mmr_bar, &ntb->bar_info[NTB_CONFIG_BAR]);
	if (rc != 0)
		return (rc);

	ntb->bar_info[NTB_B2B_BAR_1].pci_resource_id = PCIR_BAR(2);
	rc = map_pci_bar(ntb, map_memory_window_bar,
	    &ntb->bar_info[NTB_B2B_BAR_1]);
	if (rc != 0)
		return (rc);

	ntb->bar_info[NTB_B2B_BAR_2].pci_resource_id = PCIR_BAR(4);
	if (HAS_FEATURE(NTB_REGS_THRU_MW) && !HAS_FEATURE(NTB_SPLIT_BAR))
		rc = map_pci_bar(ntb, map_mmr_bar,
		    &ntb->bar_info[NTB_B2B_BAR_2]);
	else
		rc = map_pci_bar(ntb, map_memory_window_bar,
		    &ntb->bar_info[NTB_B2B_BAR_2]);
	if (!HAS_FEATURE(NTB_SPLIT_BAR))
		return (rc);

	ntb->bar_info[NTB_B2B_BAR_3].pci_resource_id = PCIR_BAR(5);
	if (HAS_FEATURE(NTB_REGS_THRU_MW))
		rc = map_pci_bar(ntb, map_mmr_bar,
		    &ntb->bar_info[NTB_B2B_BAR_3]);
	else
		rc = map_pci_bar(ntb, map_memory_window_bar,
		    &ntb->bar_info[NTB_B2B_BAR_3]);
	return (rc);
}

static int
map_pci_bar(struct ntb_softc *ntb, bar_map_strategy strategy,
    struct ntb_pci_bar_info *bar)
{
	int rc;

	rc = strategy(ntb, bar);
	if (rc != 0)
		device_printf(ntb->device,
		    "unable to allocate pci resource\n");
	else
		device_printf(ntb->device,
		    "Bar size = %lx, v %p, p %p\n",
		    bar->size, bar->vbase, (void *)(bar->pbase));
	return (rc);
}

static int
map_mmr_bar(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar)
{

	bar->pci_resource = bus_alloc_resource_any(ntb->device, SYS_RES_MEMORY,
	    &bar->pci_resource_id, RF_ACTIVE);
	if (bar->pci_resource == NULL)
		return (ENXIO);

	save_bar_parameters(bar);
	return (0);
}

static int
map_memory_window_bar(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar)
{
	int rc;
	uint8_t bar_size_bits = 0;

	bar->pci_resource = bus_alloc_resource_any(ntb->device, SYS_RES_MEMORY,
	    &bar->pci_resource_id, RF_ACTIVE);

	if (bar->pci_resource == NULL)
		return (ENXIO);

	save_bar_parameters(bar);
	/*
	 * Ivytown NTB BAR sizes are misreported by the hardware due to a
	 * hardware issue. To work around this, query the size it should be
	 * configured to by the device and modify the resource to correspond to
	 * this new size. The BIOS on systems with this problem is required to
	 * provide enough address space to allow the driver to make this change
	 * safely.
	 *
	 * Ideally I could have just specified the size when I allocated the
	 * resource like:
	 *  bus_alloc_resource(ntb->device,
	 *	SYS_RES_MEMORY, &bar->pci_resource_id, 0ul, ~0ul,
	 *	1ul << bar_size_bits, RF_ACTIVE);
	 * but the PCI driver does not honor the size in this call, so we have
	 * to modify it after the fact.
	 */
	if (HAS_FEATURE(NTB_BAR_SIZE_4K)) {
		if (bar->pci_resource_id == PCIR_BAR(2))
			bar_size_bits = pci_read_config(ntb->device,
			    XEON_PBAR23SZ_OFFSET, 1);
		else
			bar_size_bits = pci_read_config(ntb->device,
			    XEON_PBAR45SZ_OFFSET, 1);

		rc = bus_adjust_resource(ntb->device, SYS_RES_MEMORY,
		    bar->pci_resource, bar->pbase,
		    bar->pbase + (1ul << bar_size_bits) - 1);
		if (rc != 0) {
			device_printf(ntb->device,
			    "unable to resize bar\n");
			return (rc);
		}

		save_bar_parameters(bar);
	}

	/* Mark bar region as write combining to improve performance. */
	rc = pmap_change_attr((vm_offset_t)bar->vbase, bar->size,
	    VM_MEMATTR_WRITE_COMBINING);
	if (rc != 0) {
		device_printf(ntb->device,
		    "unable to mark bar as WRITE_COMBINING\n");
		return (rc);
	}
	return (0);
}

static void
ntb_unmap_pci_bar(struct ntb_softc *ntb)
{
	struct ntb_pci_bar_info *current_bar;
	int i;

	for (i = 0; i < NTB_MAX_BARS; i++) {
		current_bar = &ntb->bar_info[i];
		if (current_bar->pci_resource != NULL)
			bus_release_resource(ntb->device, SYS_RES_MEMORY,
			    current_bar->pci_resource_id,
			    current_bar->pci_resource);
	}
}

static int
ntb_setup_xeon_msix(struct ntb_softc *ntb, uint32_t num_vectors)
{
	void (*interrupt_handler)(void *);
	void *int_arg;
	uint32_t i;
	int rc;

	if (num_vectors < 4)
		return (ENOSPC);

	for (i = 0; i < num_vectors; i++) {
		ntb->int_info[i].rid = i + 1;
		ntb->int_info[i].res = bus_alloc_resource_any(ntb->device,
		    SYS_RES_IRQ, &ntb->int_info[i].rid, RF_ACTIVE);
		if (ntb->int_info[i].res == NULL) {
			device_printf(ntb->device,
			    "bus_alloc_resource failed\n");
			return (ENOMEM);
		}
		ntb->int_info[i].tag = NULL;
		ntb->allocated_interrupts++;
		if (i == num_vectors - 1) {
			interrupt_handler = handle_xeon_event_irq;
			int_arg = ntb;
		} else {
			interrupt_handler = handle_xeon_irq;
			int_arg = &ntb->db_cb[i];
		}
		rc = bus_setup_intr(ntb->device, ntb->int_info[i].res,
		    INTR_MPSAFE | INTR_TYPE_MISC, NULL, interrupt_handler,
		    int_arg, &ntb->int_info[i].tag);
		if (rc != 0) {
			device_printf(ntb->device,
			    "bus_setup_intr failed\n");
			return (ENXIO);
		}
	}

	/*
	 * Prevent consumers from registering callbacks on the link event irq
	 * slot, from which they will never be called back.
	 */
	ntb->db_cb[num_vectors - 1].reserved = true;
	ntb->max_cbs--;
	return (0);
}

static int
ntb_setup_soc_msix(struct ntb_softc *ntb, uint32_t num_vectors)
{
	uint32_t i;
	int rc;

	for (i = 0; i < num_vectors; i++) {
		ntb->int_info[i].rid = i + 1;
		ntb->int_info[i].res = bus_alloc_resource_any(ntb->device,
		    SYS_RES_IRQ, &ntb->int_info[i].rid, RF_ACTIVE);
		if (ntb->int_info[i].res == NULL) {
			device_printf(ntb->device,
			    "bus_alloc_resource failed\n");
			return (ENOMEM);
		}
		ntb->int_info[i].tag = NULL;
		ntb->allocated_interrupts++;
		rc = bus_setup_intr(ntb->device, ntb->int_info[i].res,
		    INTR_MPSAFE | INTR_TYPE_MISC, NULL, handle_soc_irq,
		    &ntb->db_cb[i], &ntb->int_info[i].tag);
		if (rc != 0) {
			device_printf(ntb->device, "bus_setup_intr failed\n");
			return (ENXIO);
		}
	}
	return (0);
}

/*
 * The Linux NTB driver drops from MSI-X to legacy INTx if a unique vector
 * cannot be allocated for each MSI-X message.  JHB seems to think remapping
 * should be okay.  This tunable should enable us to test that hypothesis
 * when someone gets their hands on some Xeon hardware.
 */
static int ntb_force_remap_mode;
SYSCTL_INT(_hw_ntb, OID_AUTO, force_remap_mode, CTLFLAG_RDTUN,
    &ntb_force_remap_mode, 0, "If enabled, force MSI-X messages to be remapped"
    " to a smaller number of ithreads, even if the desired number are "
    "available");

/*
 * In case it is NOT ok, give consumers an abort button.
 */
static int ntb_prefer_intx;
SYSCTL_INT(_hw_ntb, OID_AUTO, prefer_intx_to_remap, CTLFLAG_RDTUN,
    &ntb_prefer_intx, 0, "If enabled, prefer to use legacy INTx mode rather "
    "than remapping MSI-X messages over available slots (match Linux driver "
    "behavior)");

/*
 * Remap the desired number of MSI-X messages to available ithreads in a simple
 * round-robin fashion.
 */
static int
ntb_remap_msix(device_t dev, uint32_t desired, uint32_t avail)
{
	u_int *vectors;
	uint32_t i;
	int rc;

	if (ntb_prefer_intx != 0)
		return (ENXIO);

	vectors = malloc(desired * sizeof(*vectors), M_NTB, M_ZERO | M_WAITOK);

	for (i = 0; i < desired; i++)
		vectors[i] = (i % avail) + 1;

	rc = pci_remap_msix(dev, desired, vectors);
	free(vectors, M_NTB);
	return (rc);
}

static int
ntb_setup_interrupts(struct ntb_softc *ntb)
{
	uint32_t desired_vectors, num_vectors;
	uint64_t mask;
	int rc;

	ntb->allocated_interrupts = 0;

	/*
	 * On SOC, disable all interrupts.  On XEON, disable all but Link
	 * Interrupt.  The rest will be unmasked as callbacks are registered.
	 */
	mask = 0;
	if (ntb->type == NTB_XEON)
		mask = (1 << XEON_LINK_DB);
	db_iowrite(ntb, ntb->reg_ofs.ldb_mask, ~mask);

	num_vectors = desired_vectors = MIN(pci_msix_count(ntb->device),
	    ntb->limits.max_db_bits);
	if (desired_vectors >= 1) {
		rc = pci_alloc_msix(ntb->device, &num_vectors);

		if (ntb_force_remap_mode != 0 && rc == 0 &&
		    num_vectors == desired_vectors)
			num_vectors--;

		if (rc == 0 && num_vectors < desired_vectors) {
			rc = ntb_remap_msix(ntb->device, desired_vectors,
			    num_vectors);
			if (rc == 0)
				num_vectors = desired_vectors;
			else
				pci_release_msi(ntb->device);
		}
		if (rc != 0)
			num_vectors = 1;
	} else
		num_vectors = 1;

	/*
	 * If allocating MSI-X interrupts succeeds, limit callbacks to the
	 * number of MSI-X slots available.
	 */
	ntb_create_callbacks(ntb, num_vectors);

	if (ntb->type == NTB_XEON)
		rc = ntb_setup_xeon_msix(ntb, num_vectors);
	else
		rc = ntb_setup_soc_msix(ntb, num_vectors);
	if (rc != 0) {
		device_printf(ntb->device,
		    "Error allocating MSI-X interrupts: %d\n", rc);

		/*
		 * If allocating MSI-X interrupts failed and we're forced to
		 * use legacy INTx anyway, the only limit on individual
		 * callbacks is the number of doorbell bits.
		 *
		 * CEM: This seems odd to me but matches the behavior of the
		 * Linux driver ca. September 2013
		 */
		ntb_free_callbacks(ntb);
		ntb_create_callbacks(ntb, ntb->limits.max_db_bits);
	}

	if (ntb->type == NTB_XEON && rc == ENOSPC)
		rc = ntb_setup_legacy_interrupt(ntb);

	return (rc);
}

static int
ntb_setup_legacy_interrupt(struct ntb_softc *ntb)
{
	int rc;

	ntb->int_info[0].rid = 0;
	ntb->int_info[0].res = bus_alloc_resource_any(ntb->device, SYS_RES_IRQ,
	    &ntb->int_info[0].rid, RF_SHAREABLE|RF_ACTIVE);
	if (ntb->int_info[0].res == NULL) {
		device_printf(ntb->device, "bus_alloc_resource failed\n");
		return (ENOMEM);
	}

	ntb->int_info[0].tag = NULL;
	ntb->allocated_interrupts = 1;

	rc = bus_setup_intr(ntb->device, ntb->int_info[0].res,
	    INTR_MPSAFE | INTR_TYPE_MISC, NULL, ntb_handle_legacy_interrupt,
	    ntb, &ntb->int_info[0].tag);
	if (rc != 0) {
		device_printf(ntb->device, "bus_setup_intr failed\n");
		return (ENXIO);
	}

	return (0);
}

static void
ntb_teardown_interrupts(struct ntb_softc *ntb)
{
	struct ntb_int_info *current_int;
	int i;

	for (i = 0; i < ntb->allocated_interrupts; i++) {
		current_int = &ntb->int_info[i];
		if (current_int->tag != NULL)
			bus_teardown_intr(ntb->device, current_int->res,
			    current_int->tag);

		if (current_int->res != NULL)
			bus_release_resource(ntb->device, SYS_RES_IRQ,
			    rman_get_rid(current_int->res), current_int->res);
	}

	ntb_free_callbacks(ntb);
	pci_release_msi(ntb->device);
}

/*
 * Doorbell register and mask are 64-bit on SoC, 16-bit on Xeon.  Abstract it
 * out to make code clearer.
 */
static uint64_t
db_ioread(struct ntb_softc *ntb, uint32_t regoff)
{

	if (ntb->type == NTB_SOC)
		return (ntb_reg_read(8, regoff));

	KASSERT(ntb->type == NTB_XEON, ("bad ntb type"));

	return (ntb_reg_read(2, regoff));
}

static void
db_iowrite(struct ntb_softc *ntb, uint32_t regoff, uint64_t val)
{

	if (ntb->type == NTB_SOC) {
		ntb_reg_write(8, regoff, val);
		return;
	}

	KASSERT(ntb->type == NTB_XEON, ("bad ntb type"));
	ntb_reg_write(2, regoff, (uint16_t)val);
}

static void
mask_ldb_interrupt(struct ntb_softc *ntb, unsigned int idx)
{
	uint64_t mask;

	mask = db_ioread(ntb, ntb->reg_ofs.ldb_mask);
	mask |= 1 << (idx * ntb->bits_per_vector);
	db_iowrite(ntb, ntb->reg_ofs.ldb_mask, mask);
}

static void
unmask_ldb_interrupt(struct ntb_softc *ntb, unsigned int idx)
{
	uint64_t mask;

	mask = db_ioread(ntb, ntb->reg_ofs.ldb_mask);
	mask &= ~(1 << (idx * ntb->bits_per_vector));
	db_iowrite(ntb, ntb->reg_ofs.ldb_mask, mask);
}

static void
handle_soc_irq(void *arg)
{
	struct ntb_db_cb *db_cb = arg;
	struct ntb_softc *ntb = db_cb->ntb;

	db_iowrite(ntb, ntb->reg_ofs.ldb, (uint64_t) 1 << db_cb->db_num);

	if (db_cb->callback != NULL) {
		mask_ldb_interrupt(ntb, db_cb->db_num);
		callout_reset(&db_cb->irq_work, 0, ntb_irq_work, db_cb);
	}
}

static void
handle_xeon_irq(void *arg)
{
	struct ntb_db_cb *db_cb = arg;
	struct ntb_softc *ntb = db_cb->ntb;

	/*
	 * On Xeon, there are 16 bits in the interrupt register
	 * but only 4 vectors.  So, 5 bits are assigned to the first 3
	 * vectors, with the 4th having a single bit for link
	 * interrupts.
	 */
	db_iowrite(ntb, ntb->reg_ofs.ldb,
	    ((1 << ntb->bits_per_vector) - 1) <<
	    (db_cb->db_num * ntb->bits_per_vector));

	if (db_cb->callback != NULL) {
		mask_ldb_interrupt(ntb, db_cb->db_num);
		callout_reset(&db_cb->irq_work, 0, ntb_irq_work, db_cb);
	}
}

/* Since we do not have a HW doorbell in SOC, this is only used in JF/JT */
static void
handle_xeon_event_irq(void *arg)
{
	struct ntb_softc *ntb = arg;
	int rc;

	rc = ntb_check_link_status(ntb);
	if (rc != 0)
		device_printf(ntb->device, "Error determining link status\n");

	/* bit 15 is always the link bit */
	db_iowrite(ntb, ntb->reg_ofs.ldb, 1 << XEON_LINK_DB);
}

static void
ntb_handle_legacy_interrupt(void *arg)
{
	struct ntb_softc *ntb = arg;
	unsigned int i;
	uint64_t ldb;

	ldb = db_ioread(ntb, ntb->reg_ofs.ldb);

	if (ntb->type == NTB_XEON && (ldb & XEON_DB_HW_LINK) != 0) {
		handle_xeon_event_irq(ntb);
		ldb &= ~XEON_DB_HW_LINK;
	}

	while (ldb != 0) {
		i = ffs(ldb);
		ldb &= ldb - 1;
		if (ntb->type == NTB_SOC)
			handle_soc_irq(&ntb->db_cb[i]);
		else
			handle_xeon_irq(&ntb->db_cb[i]);
	}
}

static int
ntb_create_callbacks(struct ntb_softc *ntb, uint32_t num_vectors)
{
	uint32_t i;

	ntb->max_cbs = num_vectors;
	ntb->db_cb = malloc(num_vectors * sizeof(*ntb->db_cb), M_NTB,
	    M_ZERO | M_WAITOK);
	for (i = 0; i < num_vectors; i++) {
		ntb->db_cb[i].db_num = i;
		ntb->db_cb[i].ntb = ntb;
	}

	return (0);
}

static void
ntb_free_callbacks(struct ntb_softc *ntb)
{
	uint8_t i;

	for (i = 0; i < ntb->max_cbs; i++)
		ntb_unregister_db_callback(ntb, i);

	free(ntb->db_cb, M_NTB);
	ntb->max_cbs = 0;
}

static struct ntb_hw_info *
ntb_get_device_info(uint32_t device_id)
{
	struct ntb_hw_info *ep = pci_ids;

	while (ep->device_id) {
		if (ep->device_id == device_id)
			return (ep);
		++ep;
	}
	return (NULL);
}

static void
ntb_teardown_xeon(struct ntb_softc *ntb)
{

	ntb_hw_link_down(ntb);
}

static void
ntb_detect_max_mw(struct ntb_softc *ntb)
{

	if (ntb->type == NTB_SOC) {
		ntb->limits.max_mw = SOC_MAX_MW;
		return;
	}

	if (HAS_FEATURE(NTB_SPLIT_BAR))
		ntb->limits.max_mw = XEON_HSXSPLIT_MAX_MW;
	else
		ntb->limits.max_mw = XEON_SNB_MAX_MW;
}

static int
ntb_detect_xeon(struct ntb_softc *ntb)
{
	uint8_t ppd, conn_type;

	ppd = pci_read_config(ntb->device, NTB_PPD_OFFSET, 1);
	ntb->ppd = ppd;

	if ((ppd & XEON_PPD_DEV_TYPE) != 0)
		ntb->dev_type = NTB_DEV_USD;
	else
		ntb->dev_type = NTB_DEV_DSD;

	if ((ppd & XEON_PPD_SPLIT_BAR) != 0)
		ntb->features |= NTB_SPLIT_BAR;

	conn_type = ppd & XEON_PPD_CONN_TYPE;
	switch (conn_type) {
	case NTB_CONN_B2B:
		ntb->conn_type = conn_type;
		break;
	case NTB_CONN_RP:
	case NTB_CONN_TRANSPARENT:
	default:
		device_printf(ntb->device, "Unsupported connection type: %u\n",
		    (unsigned)conn_type);
		return (ENXIO);
	}
	return (0);
}

static int
ntb_detect_soc(struct ntb_softc *ntb)
{
	uint32_t ppd, conn_type;

	ppd = pci_read_config(ntb->device, NTB_PPD_OFFSET, 4);
	ntb->ppd = ppd;

	if ((ppd & SOC_PPD_DEV_TYPE) != 0)
		ntb->dev_type = NTB_DEV_DSD;
	else
		ntb->dev_type = NTB_DEV_USD;

	conn_type = (ppd & SOC_PPD_CONN_TYPE) >> 8;
	switch (conn_type) {
	case NTB_CONN_B2B:
		ntb->conn_type = conn_type;
		break;
	default:
		device_printf(ntb->device, "Unsupported NTB configuration\n");
		return (ENXIO);
	}
	return (0);
}

static int
ntb_setup_xeon(struct ntb_softc *ntb)
{

	ntb->reg_ofs.ldb	= XEON_PDOORBELL_OFFSET;
	ntb->reg_ofs.ldb_mask	= XEON_PDBMSK_OFFSET;
	ntb->reg_ofs.spad_local	= XEON_SPAD_OFFSET;
	ntb->reg_ofs.bar2_xlat	= XEON_SBAR2XLAT_OFFSET;
	ntb->reg_ofs.bar4_xlat	= XEON_SBAR4XLAT_OFFSET;
	if (HAS_FEATURE(NTB_SPLIT_BAR))
		ntb->reg_ofs.bar5_xlat = XEON_SBAR5XLAT_OFFSET;

	switch (ntb->conn_type) {
	case NTB_CONN_B2B:
		/*
		 * reg_ofs.rdb and reg_ofs.spad_remote are effectively ignored
		 * with the NTB_REGS_THRU_MW errata mode enabled.  (See
		 * ntb_ring_doorbell() and ntb_read/write_remote_spad().)
		 */
		ntb->reg_ofs.rdb	 = XEON_B2B_DOORBELL_OFFSET;
		ntb->reg_ofs.spad_remote = XEON_B2B_SPAD_OFFSET;

		ntb->limits.max_spads	 = XEON_MAX_SPADS;
		break;

	case NTB_CONN_RP:
		/*
		 * Every Xeon today needs NTB_REGS_THRU_MW, so punt on RP for
		 * now.
		 */
		KASSERT(HAS_FEATURE(NTB_REGS_THRU_MW),
		    ("Xeon without MW errata unimplemented"));
		device_printf(ntb->device,
		    "NTB-RP disabled to due hardware errata.\n");
		return (ENXIO);

	case NTB_CONN_TRANSPARENT:
	default:
		device_printf(ntb->device, "Connection type %d not supported\n",
		    ntb->conn_type);
		return (ENXIO);
	}

	/*
	 * There is a Xeon hardware errata related to writes to SDOORBELL or
	 * B2BDOORBELL in conjunction with inbound access to NTB MMIO space,
	 * which may hang the system.  To workaround this use the second memory
	 * window to access the interrupt and scratch pad registers on the
	 * remote system.
	 *
	 * There is another HW errata on the limit registers -- they can only
	 * be written when the base register is (?)4GB aligned and < 32-bit.
	 * This should already be the case based on the driver defaults, but
	 * write the limit registers first just in case.
	 */
	if (HAS_FEATURE(NTB_REGS_THRU_MW)) {
		/*
		 * Set the Limit register to 4k, the minimum size, to prevent
		 * an illegal access.
		 *
		 * XXX: Should this be PBAR5LMT / get_mw_size(, max_mw - 1)?
		 */
		ntb_reg_write(8, XEON_PBAR4LMT_OFFSET,
		    ntb_get_mw_size(ntb, 1) + 0x1000);
		/* Reserve the last MW for mapping remote spad */
		ntb->limits.max_mw--;
	} else
		/*
		 * Disable the limit register, just in case it is set to
		 * something silly.  A 64-bit write will also clear PBAR5LMT in
		 * split-bar mode, and this is desired.
		 */
		ntb_reg_write(8, XEON_PBAR4LMT_OFFSET, 0);

	ntb->reg_ofs.lnk_cntl	 = XEON_NTBCNTL_OFFSET;
	ntb->reg_ofs.lnk_stat	 = XEON_LINK_STATUS_OFFSET;
	ntb->reg_ofs.spci_cmd	 = XEON_PCICMD_OFFSET;

	ntb->limits.max_db_bits	 = XEON_MAX_DB_BITS;
	ntb->limits.msix_cnt	 = XEON_MSIX_CNT;
	ntb->bits_per_vector	 = XEON_DB_BITS_PER_VEC;

	/*
	 * HW Errata on bit 14 of b2bdoorbell register.  Writes will not be
	 * mirrored to the remote system.  Shrink the number of bits by one,
	 * since bit 14 is the last bit.
	 *
	 * On REGS_THRU_MW errata mode, we don't use the b2bdoorbell register
	 * anyway.  Nor for non-B2B connection types.
	 */
	if (HAS_FEATURE(NTB_B2BDOORBELL_BIT14) &&
	    !HAS_FEATURE(NTB_REGS_THRU_MW) &&
	    ntb->conn_type == NTB_CONN_B2B)
		ntb->limits.max_db_bits = XEON_MAX_DB_BITS - 1;

	configure_xeon_secondary_side_bars(ntb);

	/* Enable Bus Master and Memory Space on the secondary side */
	if (ntb->conn_type == NTB_CONN_B2B)
		ntb_reg_write(2, ntb->reg_ofs.spci_cmd,
		    PCIM_CMD_MEMEN | PCIM_CMD_BUSMASTEREN);

	/* Enable link training */
	ntb_hw_link_up(ntb);

	return (0);
}

static int
ntb_setup_soc(struct ntb_softc *ntb)
{

	KASSERT(ntb->conn_type == NTB_CONN_B2B,
	    ("Unsupported NTB configuration (%d)\n", ntb->conn_type));

	/* Initiate PCI-E link training */
	pci_write_config(ntb->device, NTB_PPD_OFFSET,
	    ntb->ppd | SOC_PPD_INIT_LINK, 4);

	ntb->reg_ofs.ldb	 = SOC_PDOORBELL_OFFSET;
	ntb->reg_ofs.ldb_mask	 = SOC_PDBMSK_OFFSET;
	ntb->reg_ofs.rdb	 = SOC_B2B_DOORBELL_OFFSET;
	ntb->reg_ofs.bar2_xlat	 = SOC_SBAR2XLAT_OFFSET;
	ntb->reg_ofs.bar4_xlat	 = SOC_SBAR4XLAT_OFFSET;
	ntb->reg_ofs.lnk_cntl	 = SOC_NTBCNTL_OFFSET;
	ntb->reg_ofs.lnk_stat	 = SOC_LINK_STATUS_OFFSET;
	ntb->reg_ofs.spad_local	 = SOC_SPAD_OFFSET;
	ntb->reg_ofs.spad_remote = SOC_B2B_SPAD_OFFSET;
	ntb->reg_ofs.spci_cmd	 = SOC_PCICMD_OFFSET;

	ntb->limits.max_spads	 = SOC_MAX_SPADS;
	ntb->limits.max_db_bits	 = SOC_MAX_DB_BITS;
	ntb->limits.msix_cnt	 = SOC_MSIX_CNT;
	ntb->bits_per_vector	 = SOC_DB_BITS_PER_VEC;

	/*
	 * FIXME - MSI-X bug on early SOC HW, remove once internal issue is
	 * resolved.  Mask transaction layer internal parity errors.
	 */
	pci_write_config(ntb->device, 0xFC, 0x4, 4);

	configure_soc_secondary_side_bars(ntb);

	/* Enable Bus Master and Memory Space on the secondary side */
	ntb_reg_write(2, ntb->reg_ofs.spci_cmd,
	    PCIM_CMD_MEMEN | PCIM_CMD_BUSMASTEREN);

	callout_reset(&ntb->heartbeat_timer, 0, ntb_handle_heartbeat, ntb);

	return (0);
}

static void
configure_soc_secondary_side_bars(struct ntb_softc *ntb)
{

	if (ntb->dev_type == NTB_DEV_USD) {
		ntb_reg_write(8, SOC_PBAR2XLAT_OFFSET, MBAR23_DSD_ADDR);
		ntb_reg_write(8, SOC_PBAR4XLAT_OFFSET, MBAR4_DSD_ADDR);
		ntb_reg_write(8, SOC_MBAR23_OFFSET, MBAR23_USD_ADDR);
		ntb_reg_write(8, SOC_MBAR45_OFFSET, MBAR4_USD_ADDR);
	} else {
		ntb_reg_write(8, SOC_PBAR2XLAT_OFFSET, MBAR23_USD_ADDR);
		ntb_reg_write(8, SOC_PBAR4XLAT_OFFSET, MBAR4_USD_ADDR);
		ntb_reg_write(8, SOC_MBAR23_OFFSET, MBAR23_DSD_ADDR);
		ntb_reg_write(8, SOC_MBAR45_OFFSET, MBAR4_DSD_ADDR);
	}
}

static void
configure_xeon_secondary_side_bars(struct ntb_softc *ntb)
{

	if (ntb->dev_type == NTB_DEV_USD) {
		ntb_reg_write(8, XEON_PBAR2XLAT_OFFSET, MBAR23_DSD_ADDR);
		if (HAS_FEATURE(NTB_REGS_THRU_MW))
			ntb_reg_write(8, XEON_PBAR4XLAT_OFFSET,
			    MBAR01_DSD_ADDR);
		else {
			if (HAS_FEATURE(NTB_SPLIT_BAR)) {
				ntb_reg_write(4, XEON_PBAR4XLAT_OFFSET,
				    MBAR4_DSD_ADDR);
				ntb_reg_write(4, XEON_PBAR5XLAT_OFFSET,
				    MBAR5_DSD_ADDR);
			} else
				ntb_reg_write(8, XEON_PBAR4XLAT_OFFSET,
				    MBAR4_DSD_ADDR);
			/*
			 * B2B_XLAT_OFFSET is a 64-bit register but can only be
			 * written 32 bits at a time.
			 */
			ntb_reg_write(4, XEON_B2B_XLAT_OFFSETL,
			    MBAR01_DSD_ADDR & 0xffffffff);
			ntb_reg_write(4, XEON_B2B_XLAT_OFFSETU,
			    MBAR01_DSD_ADDR >> 32);
		}
		ntb_reg_write(8, XEON_SBAR0BASE_OFFSET, MBAR01_USD_ADDR);
		ntb_reg_write(8, XEON_SBAR2BASE_OFFSET, MBAR23_USD_ADDR);
		if (HAS_FEATURE(NTB_SPLIT_BAR)) {
			ntb_reg_write(4, XEON_SBAR4BASE_OFFSET, MBAR4_USD_ADDR);
			ntb_reg_write(4, XEON_SBAR5BASE_OFFSET, MBAR5_USD_ADDR);
		} else
			ntb_reg_write(8, XEON_SBAR4BASE_OFFSET, MBAR4_USD_ADDR);
	} else {
		ntb_reg_write(8, XEON_PBAR2XLAT_OFFSET, MBAR23_USD_ADDR);
		if (HAS_FEATURE(NTB_REGS_THRU_MW))
			ntb_reg_write(8, XEON_PBAR4XLAT_OFFSET,
			    MBAR01_USD_ADDR);
		else {
			if (HAS_FEATURE(NTB_SPLIT_BAR)) {
				ntb_reg_write(4, XEON_PBAR4XLAT_OFFSET,
				    MBAR4_USD_ADDR);
				ntb_reg_write(4, XEON_PBAR5XLAT_OFFSET,
				    MBAR5_USD_ADDR);
			} else
				ntb_reg_write(8, XEON_PBAR4XLAT_OFFSET,
				    MBAR4_USD_ADDR);
			/*
			 * B2B_XLAT_OFFSET is a 64-bit register but can only be
			 * written 32 bits at a time.
			 */
			ntb_reg_write(4, XEON_B2B_XLAT_OFFSETL,
			    MBAR01_USD_ADDR & 0xffffffff);
			ntb_reg_write(4, XEON_B2B_XLAT_OFFSETU,
			    MBAR01_USD_ADDR >> 32);
		}
		ntb_reg_write(8, XEON_SBAR0BASE_OFFSET, MBAR01_DSD_ADDR);
		ntb_reg_write(8, XEON_SBAR2BASE_OFFSET, MBAR23_DSD_ADDR);
		if (HAS_FEATURE(NTB_SPLIT_BAR)) {
			ntb_reg_write(4, XEON_SBAR4BASE_OFFSET,
			    MBAR4_DSD_ADDR);
			ntb_reg_write(4, XEON_SBAR5BASE_OFFSET,
			    MBAR5_DSD_ADDR);
		} else
			ntb_reg_write(8, XEON_SBAR4BASE_OFFSET,
			    MBAR4_DSD_ADDR);
	}
}

/* SOC does not have link status interrupt, poll on that platform */
static void
ntb_handle_heartbeat(void *arg)
{
	struct ntb_softc *ntb = arg;
	uint32_t status32;
	int rc;

	rc = ntb_check_link_status(ntb);
	if (rc != 0)
		device_printf(ntb->device,
		    "Error determining link status\n");

	/* Check to see if a link error is the cause of the link down */
	if (ntb->link_status == NTB_LINK_DOWN) {
		status32 = ntb_reg_read(4, SOC_LTSSMSTATEJMP_OFFSET);
		if ((status32 & SOC_LTSSMSTATEJMP_FORCEDETECT) != 0) {
			callout_reset(&ntb->lr_timer, 0, recover_soc_link,
			    ntb);
			return;
		}
	}

	callout_reset(&ntb->heartbeat_timer, NTB_HB_TIMEOUT * hz,
	    ntb_handle_heartbeat, ntb);
}

static void
soc_perform_link_restart(struct ntb_softc *ntb)
{
	uint32_t status;

	/* Driver resets the NTB ModPhy lanes - magic! */
	ntb_reg_write(1, SOC_MODPHY_PCSREG6, 0xe0);
	ntb_reg_write(1, SOC_MODPHY_PCSREG4, 0x40);
	ntb_reg_write(1, SOC_MODPHY_PCSREG4, 0x60);
	ntb_reg_write(1, SOC_MODPHY_PCSREG6, 0x60);

	/* Driver waits 100ms to allow the NTB ModPhy to settle */
	pause("ModPhy", hz / 10);

	/* Clear AER Errors, write to clear */
	status = ntb_reg_read(4, SOC_ERRCORSTS_OFFSET);
	status &= PCIM_AER_COR_REPLAY_ROLLOVER;
	ntb_reg_write(4, SOC_ERRCORSTS_OFFSET, status);

	/* Clear unexpected electrical idle event in LTSSM, write to clear */
	status = ntb_reg_read(4, SOC_LTSSMERRSTS0_OFFSET);
	status |= SOC_LTSSMERRSTS0_UNEXPECTEDEI;
	ntb_reg_write(4, SOC_LTSSMERRSTS0_OFFSET, status);

	/* Clear DeSkew Buffer error, write to clear */
	status = ntb_reg_read(4, SOC_DESKEWSTS_OFFSET);
	status |= SOC_DESKEWSTS_DBERR;
	ntb_reg_write(4, SOC_DESKEWSTS_OFFSET, status);

	status = ntb_reg_read(4, SOC_IBSTERRRCRVSTS0_OFFSET);
	status &= SOC_IBIST_ERR_OFLOW;
	ntb_reg_write(4, SOC_IBSTERRRCRVSTS0_OFFSET, status);

	/* Releases the NTB state machine to allow the link to retrain */
	status = ntb_reg_read(4, SOC_LTSSMSTATEJMP_OFFSET);
	status &= ~SOC_LTSSMSTATEJMP_FORCEDETECT;
	ntb_reg_write(4, SOC_LTSSMSTATEJMP_OFFSET, status);
}

static void
ntb_handle_link_event(struct ntb_softc *ntb, int link_state)
{
	enum ntb_hw_event event;
	uint16_t status;

	if (ntb->link_status == link_state)
		return;

	if (link_state == NTB_LINK_UP) {
		device_printf(ntb->device, "Link Up\n");
		ntb->link_status = NTB_LINK_UP;
		event = NTB_EVENT_HW_LINK_UP;

		if (ntb->type == NTB_SOC ||
		    ntb->conn_type == NTB_CONN_TRANSPARENT)
			status = ntb_reg_read(2, ntb->reg_ofs.lnk_stat);
		else
			status = pci_read_config(ntb->device,
			    XEON_LINK_STATUS_OFFSET, 2);
		ntb->link_width = (status & NTB_LINK_WIDTH_MASK) >> 4;
		ntb->link_speed = (status & NTB_LINK_SPEED_MASK);
		device_printf(ntb->device, "Link Width %d, Link Speed %d\n",
		    ntb->link_width, ntb->link_speed);
		callout_reset(&ntb->heartbeat_timer, NTB_HB_TIMEOUT * hz,
		    ntb_handle_heartbeat, ntb);
	} else {
		device_printf(ntb->device, "Link Down\n");
		ntb->link_status = NTB_LINK_DOWN;
		event = NTB_EVENT_HW_LINK_DOWN;
		/* Do not modify link width/speed, we need it in link recovery */
	}

	/* notify the upper layer if we have an event change */
	if (ntb->event_cb != NULL)
		ntb->event_cb(ntb->ntb_transport, event);
}

static void
ntb_hw_link_up(struct ntb_softc *ntb)
{
	uint32_t cntl;

	if (ntb->conn_type == NTB_CONN_TRANSPARENT) {
		ntb_handle_link_event(ntb, NTB_LINK_UP);
		return;
	}

	cntl = ntb_reg_read(4, ntb->reg_ofs.lnk_cntl);
	cntl &= ~(NTB_CNTL_LINK_DISABLE | NTB_CNTL_CFG_LOCK);
	cntl |= NTB_CNTL_P2S_BAR23_SNOOP | NTB_CNTL_S2P_BAR23_SNOOP;
	cntl |= NTB_CNTL_P2S_BAR4_SNOOP | NTB_CNTL_S2P_BAR4_SNOOP;
	if (HAS_FEATURE(NTB_SPLIT_BAR))
		cntl |= NTB_CNTL_P2S_BAR5_SNOOP | NTB_CNTL_S2P_BAR5_SNOOP;
	ntb_reg_write(4, ntb->reg_ofs.lnk_cntl, cntl);
}

static void
ntb_hw_link_down(struct ntb_softc *ntb)
{
	uint32_t cntl;

	if (ntb->conn_type == NTB_CONN_TRANSPARENT) {
		ntb_handle_link_event(ntb, NTB_LINK_DOWN);
		return;
	}

	cntl = ntb_reg_read(4, ntb->reg_ofs.lnk_cntl);
	cntl &= ~(NTB_CNTL_P2S_BAR23_SNOOP | NTB_CNTL_S2P_BAR23_SNOOP);
	cntl &= ~(NTB_CNTL_P2S_BAR4_SNOOP | NTB_CNTL_S2P_BAR4_SNOOP);
	if (HAS_FEATURE(NTB_SPLIT_BAR))
		cntl &= ~(NTB_CNTL_P2S_BAR5_SNOOP | NTB_CNTL_S2P_BAR5_SNOOP);
	cntl |= NTB_CNTL_LINK_DISABLE | NTB_CNTL_CFG_LOCK;
	ntb_reg_write(4, ntb->reg_ofs.lnk_cntl, cntl);
}

static void
recover_soc_link(void *arg)
{
	struct ntb_softc *ntb = arg;
	uint8_t speed, width;
	uint32_t status32;
	uint16_t status16;

	soc_perform_link_restart(ntb);

	/*
	 * There is a potential race between the 2 NTB devices recovering at
	 * the same time.  If the times are the same, the link will not recover
	 * and the driver will be stuck in this loop forever.  Add a random
	 * interval to the recovery time to prevent this race.
	 */
	status32 = arc4random() % SOC_LINK_RECOVERY_TIME;
	pause("Link", (SOC_LINK_RECOVERY_TIME + status32) * hz / 1000);

	status32 = ntb_reg_read(4, SOC_LTSSMSTATEJMP_OFFSET);
	if ((status32 & SOC_LTSSMSTATEJMP_FORCEDETECT) != 0)
		goto retry;

	status32 = ntb_reg_read(4, SOC_IBSTERRRCRVSTS0_OFFSET);
	if ((status32 & SOC_IBIST_ERR_OFLOW) != 0)
		goto retry;

	status32 = ntb_reg_read(4, ntb->reg_ofs.lnk_cntl);
	if ((status32 & SOC_CNTL_LINK_DOWN) != 0)
		goto out;

	status16 = ntb_reg_read(2, ntb->reg_ofs.lnk_stat);
	width = (status16 & NTB_LINK_WIDTH_MASK) >> 4;
	speed = (status16 & NTB_LINK_SPEED_MASK);
	if (ntb->link_width != width || ntb->link_speed != speed)
		goto retry;

out:
	callout_reset(&ntb->heartbeat_timer, NTB_HB_TIMEOUT * hz,
	    ntb_handle_heartbeat, ntb);
	return;

retry:
	callout_reset(&ntb->lr_timer, NTB_HB_TIMEOUT * hz, recover_soc_link,
	    ntb);
}

static int
ntb_check_link_status(struct ntb_softc *ntb)
{
	int link_state;
	uint32_t ntb_cntl;
	uint16_t status;

	if (ntb->type == NTB_SOC) {
		ntb_cntl = ntb_reg_read(4, ntb->reg_ofs.lnk_cntl);
		if ((ntb_cntl & SOC_CNTL_LINK_DOWN) != 0)
			link_state = NTB_LINK_DOWN;
		else
			link_state = NTB_LINK_UP;
	} else {
		status = pci_read_config(ntb->device, XEON_LINK_STATUS_OFFSET,
		    2);

		if ((status & NTB_LINK_STATUS_ACTIVE) != 0)
			link_state = NTB_LINK_UP;
		else
			link_state = NTB_LINK_DOWN;
	}

	ntb_handle_link_event(ntb, link_state);

	return (0);
}

/**
 * ntb_register_event_callback() - register event callback
 * @ntb: pointer to ntb_softc instance
 * @func: callback function to register
 *
 * This function registers a callback for any HW driver events such as link
 * up/down, power management notices and etc.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_register_event_callback(struct ntb_softc *ntb, ntb_event_callback func)
{

	if (ntb->event_cb != NULL)
		return (EINVAL);

	ntb->event_cb = func;

	return (0);
}

/**
 * ntb_unregister_event_callback() - unregisters the event callback
 * @ntb: pointer to ntb_softc instance
 *
 * This function unregisters the existing callback from transport
 */
void
ntb_unregister_event_callback(struct ntb_softc *ntb)
{

	ntb->event_cb = NULL;
}

static void
ntb_irq_work(void *arg)
{
	struct ntb_db_cb *db_cb = arg;
	struct ntb_softc *ntb;
	int rc;

	rc = db_cb->callback(db_cb->data, db_cb->db_num);
	/* Poll if forward progress was made. */
	if (rc != 0) {
		callout_reset(&db_cb->irq_work, 0, ntb_irq_work, db_cb);
		return;
	}

	/* Unmask interrupt if no progress was made. */
	ntb = db_cb->ntb;
	unmask_ldb_interrupt(ntb, db_cb->db_num);
}

/**
 * ntb_register_db_callback() - register a callback for doorbell interrupt
 * @ntb: pointer to ntb_softc instance
 * @idx: doorbell index to register callback, zero based
 * @data: pointer to be returned to caller with every callback
 * @func: callback function to register
 *
 * This function registers a callback function for the doorbell interrupt
 * on the primary side. The function will unmask the doorbell as well to
 * allow interrupt.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_register_db_callback(struct ntb_softc *ntb, unsigned int idx, void *data,
    ntb_db_callback func)
{
	struct ntb_db_cb *db_cb = &ntb->db_cb[idx];

	if (idx >= ntb->max_cbs || db_cb->callback != NULL || db_cb->reserved) {
		device_printf(ntb->device, "Invalid Index.\n");
		return (EINVAL);
	}

	db_cb->callback = func;
	db_cb->data = data;
	callout_init(&db_cb->irq_work, 1);

	unmask_ldb_interrupt(ntb, idx);

	return (0);
}

/**
 * ntb_unregister_db_callback() - unregister a callback for doorbell interrupt
 * @ntb: pointer to ntb_softc instance
 * @idx: doorbell index to register callback, zero based
 *
 * This function unregisters a callback function for the doorbell interrupt
 * on the primary side. The function will also mask the said doorbell.
 */
void
ntb_unregister_db_callback(struct ntb_softc *ntb, unsigned int idx)
{

	if (idx >= ntb->max_cbs || ntb->db_cb[idx].callback == NULL)
		return;

	mask_ldb_interrupt(ntb, idx);

	callout_drain(&ntb->db_cb[idx].irq_work);
	ntb->db_cb[idx].callback = NULL;
}

/**
 * ntb_find_transport() - find the transport pointer
 * @transport: pointer to pci device
 *
 * Given the pci device pointer, return the transport pointer passed in when
 * the transport attached when it was inited.
 *
 * RETURNS: pointer to transport.
 */
void *
ntb_find_transport(struct ntb_softc *ntb)
{

	return (ntb->ntb_transport);
}

/**
 * ntb_register_transport() - Register NTB transport with NTB HW driver
 * @transport: transport identifier
 *
 * This function allows a transport to reserve the hardware driver for
 * NTB usage.
 *
 * RETURNS: pointer to ntb_softc, NULL on error.
 */
struct ntb_softc *
ntb_register_transport(struct ntb_softc *ntb, void *transport)
{

	/*
	 * TODO: when we have more than one transport, we will need to rewrite
	 * this to prevent race conditions
	 */
	if (ntb->ntb_transport != NULL)
		return (NULL);

	ntb->ntb_transport = transport;
	return (ntb);
}

/**
 * ntb_unregister_transport() - Unregister the transport with the NTB HW driver
 * @ntb - ntb_softc of the transport to be freed
 *
 * This function unregisters the transport from the HW driver and performs any
 * necessary cleanups.
 */
void
ntb_unregister_transport(struct ntb_softc *ntb)
{
	uint8_t i;

	if (ntb->ntb_transport == NULL)
		return;

	for (i = 0; i < ntb->max_cbs; i++)
		ntb_unregister_db_callback(ntb, i);

	ntb_unregister_event_callback(ntb);
	ntb->ntb_transport = NULL;
}

/**
 * ntb_get_max_spads() - get the total scratch regs usable
 * @ntb: pointer to ntb_softc instance
 *
 * This function returns the max 32bit scratchpad registers usable by the
 * upper layer.
 *
 * RETURNS: total number of scratch pad registers available
 */
uint8_t
ntb_get_max_spads(struct ntb_softc *ntb)
{

	return (ntb->limits.max_spads);
}

uint8_t
ntb_get_max_cbs(struct ntb_softc *ntb)
{

	return (ntb->max_cbs);
}

uint8_t
ntb_get_max_mw(struct ntb_softc *ntb)
{

	return (ntb->limits.max_mw);
}

/**
 * ntb_write_local_spad() - write to the secondary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to the scratchpad register, 0 based
 * @val: the data value to put into the register
 *
 * This function allows writing of a 32bit value to the indexed scratchpad
 * register. The register resides on the secondary (external) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_write_local_spad(struct ntb_softc *ntb, unsigned int idx, uint32_t val)
{

	if (idx >= ntb->limits.max_spads)
		return (EINVAL);

	ntb_reg_write(4, ntb->reg_ofs.spad_local + idx * 4, val);

	return (0);
}

/**
 * ntb_read_local_spad() - read from the primary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to scratchpad register, 0 based
 * @val: pointer to 32bit integer for storing the register value
 *
 * This function allows reading of the 32bit scratchpad register on
 * the primary (internal) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_read_local_spad(struct ntb_softc *ntb, unsigned int idx, uint32_t *val)
{

	if (idx >= ntb->limits.max_spads)
		return (EINVAL);

	*val = ntb_reg_read(4, ntb->reg_ofs.spad_local + idx * 4);

	return (0);
}

/**
 * ntb_write_remote_spad() - write to the secondary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to the scratchpad register, 0 based
 * @val: the data value to put into the register
 *
 * This function allows writing of a 32bit value to the indexed scratchpad
 * register. The register resides on the secondary (external) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_write_remote_spad(struct ntb_softc *ntb, unsigned int idx, uint32_t val)
{

	if (idx >= ntb->limits.max_spads)
		return (EINVAL);

	if (HAS_FEATURE(NTB_REGS_THRU_MW))
		ntb_mw_write(4, XEON_SHADOW_SPAD_OFFSET + idx * 4, val);
	else
		ntb_reg_write(4, ntb->reg_ofs.spad_remote + idx * 4, val);

	return (0);
}

/**
 * ntb_read_remote_spad() - read from the primary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to scratchpad register, 0 based
 * @val: pointer to 32bit integer for storing the register value
 *
 * This function allows reading of the 32bit scratchpad register on
 * the primary (internal) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_read_remote_spad(struct ntb_softc *ntb, unsigned int idx, uint32_t *val)
{

	if (idx >= ntb->limits.max_spads)
		return (EINVAL);

	if (HAS_FEATURE(NTB_REGS_THRU_MW))
		*val = ntb_mw_read(4, XEON_SHADOW_SPAD_OFFSET + idx * 4);
	else
		*val = ntb_reg_read(4, ntb->reg_ofs.spad_remote + idx * 4);

	return (0);
}

/**
 * ntb_get_mw_vbase() - get virtual addr for the NTB memory window
 * @ntb: pointer to ntb_softc instance
 * @mw: memory window number
 *
 * This function provides the base virtual address of the memory window
 * specified.
 *
 * RETURNS: pointer to virtual address, or NULL on error.
 */
void *
ntb_get_mw_vbase(struct ntb_softc *ntb, unsigned int mw)
{

	if (mw >= ntb_get_max_mw(ntb))
		return (NULL);

	return (ntb->bar_info[NTB_MW_TO_BAR(mw)].vbase);
}

vm_paddr_t
ntb_get_mw_pbase(struct ntb_softc *ntb, unsigned int mw)
{

	if (mw >= ntb_get_max_mw(ntb))
		return (0);

	return (ntb->bar_info[NTB_MW_TO_BAR(mw)].pbase);
}

/**
 * ntb_get_mw_size() - return size of NTB memory window
 * @ntb: pointer to ntb_softc instance
 * @mw: memory window number
 *
 * This function provides the physical size of the memory window specified
 *
 * RETURNS: the size of the memory window or zero on error
 */
u_long
ntb_get_mw_size(struct ntb_softc *ntb, unsigned int mw)
{

	if (mw >= ntb_get_max_mw(ntb))
		return (0);

	return (ntb->bar_info[NTB_MW_TO_BAR(mw)].size);
}

/**
 * ntb_set_mw_addr - set the memory window address
 * @ntb: pointer to ntb_softc instance
 * @mw: memory window number
 * @addr: base address for data
 *
 * This function sets the base physical address of the memory window.  This
 * memory address is where data from the remote system will be transfered into
 * or out of depending on how the transport is configured.
 */
void
ntb_set_mw_addr(struct ntb_softc *ntb, unsigned int mw, uint64_t addr)
{

	if (mw >= ntb_get_max_mw(ntb))
		return;

	switch (NTB_MW_TO_BAR(mw)) {
	case NTB_B2B_BAR_1:
		ntb_reg_write(8, ntb->reg_ofs.bar2_xlat, addr);
		break;
	case NTB_B2B_BAR_2:
		if (HAS_FEATURE(NTB_SPLIT_BAR))
			ntb_reg_write(4, ntb->reg_ofs.bar4_xlat, addr);
		else
			ntb_reg_write(8, ntb->reg_ofs.bar4_xlat, addr);
		break;
	case NTB_B2B_BAR_3:
		ntb_reg_write(4, ntb->reg_ofs.bar5_xlat, addr);
		break;
	}
}

/**
 * ntb_ring_doorbell() - Set the doorbell on the secondary/external side
 * @ntb: pointer to ntb_softc instance
 * @db: doorbell to ring
 *
 * This function allows triggering of a doorbell on the secondary/external
 * side that will initiate an interrupt on the remote host
 */
void
ntb_ring_doorbell(struct ntb_softc *ntb, unsigned int db)
{
	uint64_t bit;

	if (ntb->type == NTB_SOC)
		bit = 1 << db;
	else
		bit = ((1 << ntb->bits_per_vector) - 1) <<
		    (db * ntb->bits_per_vector);

	if (HAS_FEATURE(NTB_REGS_THRU_MW)) {
		ntb_mw_write(2, XEON_SHADOW_PDOORBELL_OFFSET, bit);
		return;
	}

	db_iowrite(ntb, ntb->reg_ofs.rdb, bit);
}

/**
 * ntb_query_link_status() - return the hardware link status
 * @ndev: pointer to ntb_device instance
 *
 * Returns true if the hardware is connected to the remote system
 *
 * RETURNS: true or false based on the hardware link state
 */
bool
ntb_query_link_status(struct ntb_softc *ntb)
{

	return (ntb->link_status == NTB_LINK_UP);
}

static void
save_bar_parameters(struct ntb_pci_bar_info *bar)
{

	bar->pci_bus_tag = rman_get_bustag(bar->pci_resource);
	bar->pci_bus_handle = rman_get_bushandle(bar->pci_resource);
	bar->pbase = rman_get_start(bar->pci_resource);
	bar->size = rman_get_size(bar->pci_resource);
	bar->vbase = rman_get_virtual(bar->pci_resource);
}

device_t
ntb_get_device(struct ntb_softc *ntb)
{

	return (ntb->device);
}

/* Export HW-specific errata information. */
bool
ntb_has_feature(struct ntb_softc *ntb, uint64_t feature)
{

	return (HAS_FEATURE(feature));
}
