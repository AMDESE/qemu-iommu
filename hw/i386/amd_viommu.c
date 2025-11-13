/*
 * QEMU support of AMD HW-assisted VIOMMU
 *
 * Copyright (C) 2021 Suravee Suthikulpanit <suravee.suthikulpanit@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Implementation inspired by hw/i386/intel_iommu.c
 *
 */

#include <sys/ioctl.h>
#include <linux/amd_viommu.h>

#include "qemu/osdep.h"
#include "hw/i386/pc.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci-internal.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-bridge/pci_expander_bridge.h"

#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "amd_iommu.h"
#include "amd_iommu_helper.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/i386/apic_internal.h"
#include "trace.h"
#include "hw/i386/apic-msidef.h"
#include "system/kvm_int.h"

#include "system/runstate.h"
#include "system/iommufd.h"

struct amd_as_key {
    PCIBus *bus;
    uint8_t devfn;
    uint32_t pasid;
};

static void amd_viommu_vm_state_change(void *opaque,
                                        bool running, RunState state);

/* ------------- IOCTL helpers  -------------*/

static int amd_viommu_mmio_write(AMDVIState *s, __u32 offset,
                                 __u32 size, __u64 value)
{
    int ret;
    struct amd_viommu_mmio_data arg = {
        .size = sizeof(arg),
    };
    uint16_t bdf = PCI_BUILD_BDF((s->iommu.host.bus),
				PCI_DEVFN(s->iommu.host.slot,
					  s->iommu.host.function));
    arg.iommu_devid = bdf;
    arg.gid = s->gid;
    arg.offset = offset;
    arg.mmio_size = size;
    arg.value = value;
    arg.is_write = true;

    return ret = ioctl(s->iommufd->fd, VIOMMU_MMIO_ACCESS, &arg);
}

static int amd_viommu_mmio_read(AMDVIState *s, __u32 offset,
                                __u32 size, __u64 *value)
{
    int ret;
    struct amd_viommu_mmio_data arg = {
        .size = sizeof(arg),
    };
    uint16_t bdf = PCI_BUILD_BDF((s->iommu.host.bus),
				PCI_DEVFN(s->iommu.host.slot,
					  s->iommu.host.function));
    arg.iommu_devid = bdf;
    arg.gid = s->gid;
    arg.offset = offset;
    arg.mmio_size = size;
    arg.value = 0;
    arg.is_write = false;

    ret = ioctl(s->iommufd->fd, VIOMMU_MMIO_ACCESS, &arg);
    if (!ret && value)
        *value = arg.value;

    return ret;
}

static uint64_t amdvi_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AMDVIState *s = opaque;
    uint64_t val = -1;
    int ret;

    if (addr + size > AMD_VIOMMU_MMIO_SIZE) {
        trace_amdvi_mmio_read_invalid(AMD_VIOMMU_MMIO_SIZE, addr, size);
        return (uint64_t)-1;
    }

    ret = amd_viommu_mmio_read(s, (addr & ~0x07), size, (__u64 *)&val);
    if (ret)
        val = -1;

    return val;
}

static int amd_viommu_update_gcr3(AMDVIState *s, AMDIOMMUFDDevice *dev,
				  uint16_t dev_id, uint64_t *dte)
{
    Error *local_err = NULL;
    int ret = -EINVAL;
    uint32_t hwpt_id;
    struct iommu_hwpt_amd_guest hwpt;
    VFIODevice *vdev;

    if (!dev)
        goto out;

    vdev = dev->hiod->agent;

    hwpt.dte[0] = dte[0];
    hwpt.dte[1] = dte[1];
    hwpt.dte[2] = dte[2];
    hwpt.dte[3] = dte[3];

    fprintf(stderr, "DEBUG: %s: ALLOC , gdevid=%#x, dte=%#016lx:%016lx:%016lx:%016lx\n",
		__func__, dev_id, dte[0], dte[1], dte[2], dte[3]);

    /*
     * TODO:
     *  - Destroy already allocated nested page table?
     *  - Detach hwpt/destroy nested domain unset_iommu_device path as well?
     */
    /* Calling drivers/iommu/amd/viommu.c: _amd_viommu_alloc_domain_nested() */
    ret = iommufd_backend_alloc_hwpt(s->iommufd,
                                     vdev->idev.dev_id,
                                     s->core->viommu_id,
                                     0,
                                     IOMMU_HWPT_DATA_AMD_GUEST,
                                     sizeof(hwpt), &hwpt, &hwpt_id, &local_err);
    if (!ret)
        return -EINVAL;

    dev->v2_hwpt_id = hwpt_id;

    fprintf(stderr, "DEBUG: %s: ATTACH, idev.dev_id=%#x, hwpt_id=%#x:%#x\n",
	    __func__, vdev->idev.dev_id, dev->v1_hwpt.hwpt_id, hwpt_id);

    ret = iommufd_device_attach_hwpt(&vdev->idev, hwpt_id);

out:
    return ret;
}

static uint64_t amd_viommu_dte_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t val = 0;
    AMDVIState *s = opaque;

    if (size ==  2) {
        val = lduw_le_p(&s->devtab[offset]);
    } else if (size == 4) {
        val = ldl_le_p(&s->devtab[offset]);
    } else if (size == 8) {
        val = ldq_le_p(&s->devtab[offset]);
    }

    return val;
}

/* TODO: Find better way to track device attachment count */
static void amd_viommu_put_v1_hwpt(AMDIOMMUFDDevice *dev, uint32_t devid, AMDVIState *s)
{
    int hwpt_id = dev->v1_hwpt.hwpt_id;
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(dev->hiod);

    if (host_iommu_device_iommufd_attach_hwpt(idev, idev->ioas_id, NULL)) {
        fprintf(stderr, "Failed to unattach device\n");
    }

    s->hwpt_cnt--;
    if (s->hwpt_cnt == 0)
        iommufd_backend_free_id(dev->iommu_state->iommufd, hwpt_id);
}

static int amd_viommu_get_v1_hwpt(AMDIOMMUFDDevice *dev, uint32_t devid, AMDVIState *s)
{
    Error *local_err = NULL;
    int ret;
    uint32_t hwpt_id;
    VFIODevice *vdev = dev->hiod->agent;
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(dev->hiod);

    fprintf(stderr, "DEBUG: %s: devid=%#x\n", __func__, devid);

//SURAVEE: TODO: MOVE THIS
    /* Allocated nested parent domain */
    if (s->hwpt_cnt == 0) {
        ret = iommufd_backend_alloc_hwpt(dev->iommu_state->iommufd,
                                         vdev->idev.dev_id,
                                         idev->ioas_id,
                                         IOMMU_HWPT_ALLOC_NEST_PARENT,
                                         IOMMU_HWPT_DATA_NONE,
                                         0, NULL, &hwpt_id, &local_err);
        if (!ret) {
            fprintf(stderr, "%s: iommufd_backend_alloc_hwpt failed\n", __func__);
            return -EINVAL;
        }

        dev->v1_hwpt.hwpt_id = hwpt_id;
        dev->v1_hwpt.parent_ioas_id = idev->ioas_id;
    }

    /* Attached device to nested parent domain */
    if (!host_iommu_device_iommufd_attach_hwpt(idev, dev->v1_hwpt.hwpt_id, &local_err)) {
	    fprintf(stderr, "%s: Attach_hwpt failed for devid 0x%x\n",
                    __func__, vdev->idev.dev_id);
	    return -EINVAL;
    }
    s->hwpt_cnt++;

    return 0;
}

//SURAVEE: FIXME
static AMDIOMMUFDDevice *amd_viommu_get_device_from_bdf(AMDVIState *s, uint16_t bus, uint16_t devfn)
{
    AMDIOMMUFDDevice *amd_idev;
    struct amd_as_key *key;
    GHashTableIter as_it;

    g_hash_table_iter_init(&as_it, s->amd_iommufd_dev_hash);

    while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
        if (pci_bus_num(key->bus) == bus && key->devfn == devfn)
            return amd_idev;
    }
    return NULL;
}

static void amd_viommu_dte_write(void *opaque, hwaddr offset, uint64_t val,
                             unsigned size)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *dev;
    uint64_t dte[4];
    uint64_t dte0, dte1, offset0 = 0, offset1 = 0, offset2 = 0, offset3 = 0;
    uint32_t devid;
    uint32_t domid = -1;

    if (size ==  2) {
        stw_le_p(&s->devtab[offset], val);
    } else if (size == 4) {
        stl_le_p(&s->devtab[offset], val);
    } else if (size == 8) {
        stq_le_p(&s->devtab[offset], val);
    }

    devid = offset >> 5;
    if (devid >= AMDVI_DEVID_MAX) {
	error_printf("amd_viommu: Invalid device id (%#x)", devid);
        return;
    }

    /*
     * Note:
     * For now, we only care about the case when writing to
     * DTE[1] (for DomainID, GCR3 Table Root Pointer)
     * DTE[2] (for GuestPagingMode).
     */
    if (offset % 0x20 == 0) {
        return; /* Ignore DTE[0] */
    } else if (offset % 0x20 == 0x8) {
	fprintf(stderr, "DEBUG: %s offset1=%#llx, val=%#lx\n", __func__, (unsigned long long )offset, val);
	offset3 = offset + 0x10;
	offset2 = offset + 0x8;
	offset1 = offset;
	offset0 = offset - 0x8;
    } else if (offset % 0x20 == 0x10) {
	fprintf(stderr, "DEBUG: %s offset2=%#llx, val=%#lx\n", __func__, (unsigned long long )offset, val);
	offset3 = offset + 0x8;
	offset2 = offset;
	offset1 = offset - 0x8;
	offset0 = offset - 0x10;
    } else if (offset % 0x20 == 0x18) {
	fprintf(stderr, "DEBUG: %s offset3=%#llx, val=%#lx\n", __func__, (unsigned long long )offset, val);
	offset3 = offset;
	offset2 = offset - 0x8;
	offset1 = offset - 0x10;
	offset0 = offset - 0x18;
    }

    dte0 = amd_viommu_dte_read(opaque, offset0, size);
    dte1 = amd_viommu_dte_read(opaque, offset1, size);

    if (dte0 & 0xE03ULL) {
        domid = dte1 & 0xFFFFULL;
	/* TODO: Move s->dev_domid to AMDIOMMUFDDevice structure */
        int tmp = s->dev_domid[devid];

	if (tmp == domid)
		return;

	s->dev_domid[devid] = domid;
	trace_amd_viommu_dte(s->devtab_base + offset0, s->devtab_base + offset1,
                             size, val, devid, domid);
    }

   /* FIXME: Currently, we iterate through all devices instead of g_hash_table_lookup()
    * since we cannot get PCIBus from bus number to use it as key
    *
    * TODO: Use bus number itself in key as bus number is used in comparision
    */
     devid = (pci_bus_num(s->primary_bus) << 8) + devid;
     dev = amd_viommu_get_device_from_bdf(s, devid >> 8, devid & 0xFF);
     if (!dev) {
        fprintf(stderr, "DEBUG: %s: %u: Failed get_device_from_bdf\n", __func__, __LINE__);
	exit(-EINVAL);
     }

        dte[0] = amd_viommu_dte_read(opaque, offset0, size);
        dte[1] = amd_viommu_dte_read(opaque, offset1, size);
        dte[2] = amd_viommu_dte_read(opaque, offset2, size);
        if (dte[2] == 0xffffffffffffffff)
            dte[2] = 0;
        dte[3] = amd_viommu_dte_read(opaque, offset3, size);
        if (dte[3] == 0xffffffffffffffff)
            dte[3] = 0;
        amd_viommu_update_gcr3(s, dev, devid, dte);
}

static const MemoryRegionOps dte_ops = {
    .read = amd_viommu_dte_read,
    .write = amd_viommu_dte_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = false,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    }
};

/* TODO: Call this function only first time for each IOMMU instance? */
static inline void amdvi_handle_devtab_mmio_write(AMDVIState *s)
{
    char name[30];
    uint64_t offset;
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_DEVICE_TABLE);

    snprintf(name, 30, "%s-%02u", "amd-iommu-devtab", s->iommu.id);
    s->devtab_base = (val & AMDVI_MMIO_DEVTAB_BASE_MASK);
    s->devtab_len = ((s->last_bus_nr - pci_bus_num(s->primary_bus)) << 8) | 0xFF;
    s->devtab_len *= 0x20;
    offset = s->devtab_base + ((pci_bus_num(s->primary_bus) << 8) * 0x20);

    /*
     * Set up memory notifier for IOMMU Device Table
     */
    fprintf(stderr, "DEBUG: %s: %s, base=%lx, offset=%#lx, bus=%#x, last_bus=%#x, len=%#lx\n", __func__,
	name, s->devtab_base, offset,
        pci_bus_num(s->primary_bus), s->last_bus_nr, s->devtab_len);

    /*
     * DTE invalidation commands are acceleration (3rd 4K MMIO space). Hence trap
     * DTE memory region write operation
     */
    memory_region_init_io(&s->devtab_mr, OBJECT(s), &dte_ops, s, name, s->devtab_len);
    memory_region_add_subregion_overlap(get_system_memory(), offset, &s->devtab_mr, 1);
}

static inline void amdvi_handle_control_write(AMDVIState *s)
{
    unsigned long val = amdvi_readq(s, AMDVI_MMIO_CONTROL);

    amd_viommu_mmio_write(s, AMDVI_MMIO_CONTROL, 8, val);
}

static inline void amdvi_handle_cmdbase_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_COMMAND_BASE);
    uint64_t addr = (val & 0xFFFFFFFFFF000ULL);
    uint64_t len = (val >> 56) & 0xF;

fprintf(stderr, "DEBUG: %s: addr=%#lx, len=%#lx\n",
	__func__, addr, len);

    s->cmdbuf_hwq = iommufd_viommu_alloc_hw_queue(s->core,
                                          IOMMU_HW_QUEUE_TYPE_AMD_CMD,
                                          0, addr, len);
    if (!s->cmdbuf_hwq) {
        error_report("%s: gid:%#x: failed to allocate command buffer\n",
                     __func__, s->gid);
    }
}

static inline void amdvi_handle_evtbase_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_EVENT_BASE);
    uint64_t addr = (val & 0xFFFFFFFFFF000ULL);
    uint64_t len = (val >> 56) & 0xF;

fprintf(stderr, "DEBUG: %s: addr=%#lx, len=%#lx\n",
	__func__, addr, len);

    s->evtlog_hwq = iommufd_viommu_alloc_hw_queue(s->core,
                                          IOMMU_HW_QUEUE_TYPE_AMD_EVT,
                                          0, addr, len);
    if (!s->evtlog_hwq) {
        error_report("%s: gid:%#x: failed to allocate event log\n",
                     __func__, s->gid);
    }
}

static inline void amdvi_handle_pprbase_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_PPR_BASE);
    uint64_t addr = (val & 0xFFFFFFFFFF000ULL);
    uint64_t len = (val >> 56) & 0xF;

fprintf(stderr, "DEBUG: %s: addr=%#lx, len=%#lx\n",
	__func__, addr, len);

    s->pprlog_hwq = iommufd_viommu_alloc_hw_queue(s->core,
                                          IOMMU_HW_QUEUE_TYPE_AMD_PPR,
                                          0, addr, len);
    if (!s->pprlog_hwq) {
        error_report("%s: gid:%#x: failed to allocate ppr log\n",
                     __func__, s->gid);
    }
}

static inline void amdvi_handle_xt_event_int_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_XT_EVENT_INT);

fprintf(stderr, "DEBUG: %s\n", __func__);
    amd_viommu_mmio_write(s, AMDVI_MMIO_XT_EVENT_INT, 8, val);
}

static inline void amdvi_handle_xt_ppr_int_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_XT_PPR_INT);

fprintf(stderr, "DEBUG: %s\n", __func__);
    amd_viommu_mmio_write(s, AMDVI_MMIO_XT_PPR_INT, 8, val);
}

/* FIXME: something might go wrong if System Software writes in chunks
 * of one byte but linux writes in chunks of 4 bytes so currently it
 * works correctly with linux but will definitely be busted if software
 * reads/writes 8 bytes
 */
static void amdvi_mmio_reg_write(AMDVIState *s, unsigned size, uint64_t val,
                                 hwaddr addr)
{
    if (size == 2) {
        amdvi_writew(s, addr, val);
    } else if (size == 4) {
        amdvi_writel(s, addr, val);
    } else if (size == 8) {
        amdvi_writeq(s, addr, val);
    }
}

static void amdvi_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    AMDVIState *s = opaque;
    unsigned long offset = addr & 0x07;

    if (addr + size > AMD_VIOMMU_MMIO_SIZE) {
        trace_amdvi_mmio_write("error: addr outside region: max ",
                (uint64_t)AMD_VIOMMU_MMIO_SIZE, size, val, offset);
        return;
    }

    switch (addr & ~0x07) {
    case AMDVI_MMIO_CONTROL:
        amdvi_mmio_reg_write(s, size, val, addr);
        amdvi_handle_control_write(s);
        break;
    case AMDVI_MMIO_DEVICE_TABLE:
        amdvi_mmio_reg_write(s, size, val, addr);
       /*  set device table address
        *   This also suffers from inability to tell whether software
        *   is done writing
        */
        if (offset || (size == 8)) {
            amdvi_handle_devtab_mmio_write(s);
        }
        break;
    case AMDVI_MMIO_COMMAND_BASE:
        amdvi_mmio_reg_write(s, size, val, addr);
        /* FIXME - make sure System Software has finished writing incase
         * it writes in chucks less than 8 bytes in a robust way.As for
         * now, this hacks works for the linux driver
         */
        if (offset || (size == 8)) {
            amdvi_handle_cmdbase_write(s);
        }
        break;
    case AMDVI_MMIO_EVENT_BASE:
        amdvi_mmio_reg_write(s, size, val, addr);
        if (offset || (size == 8)) {
            amdvi_handle_evtbase_write(s);
        }
        break;
    case AMDVI_MMIO_PPR_BASE:
        amdvi_mmio_reg_write(s, size, val, addr);
        if (offset || (size == 8)) {
            amdvi_handle_pprbase_write(s);
        }
        break;
    case AMDVI_MMIO_XT_EVENT_INT:
        amdvi_mmio_reg_write(s, size, val, addr);
        amdvi_handle_xt_event_int_write(s);
        break;
    case AMDVI_MMIO_XT_PPR_INT:
        amdvi_mmio_reg_write(s, size, val, addr);
        amdvi_handle_xt_ppr_int_write(s);
        break;
    }
}

static AddressSpace *amd_viommu_host_dma_iommu(PCIBus *bus, void *opaque, int devfn)
{
    char name[128];
    AMDVIState *s = opaque;
    AMDVIAddressSpace **iommu_as, *amdvi_dev_as;
    int bus_num = pci_bus_num(bus);

    iommu_as = s->address_spaces[bus_num];

    /* allocate memory during the first run */
    if (!iommu_as) {
        iommu_as = g_malloc0(sizeof(AMDVIAddressSpace *) * PCI_DEVFN_MAX);
        s->address_spaces[bus_num] = iommu_as;
    }

    /* set up AMD-Vi region */
    if (!iommu_as[devfn]) {
        snprintf(name, sizeof(name), "amd_iommu_devfn_%d", devfn);

        iommu_as[devfn] = g_malloc0(sizeof(AMDVIAddressSpace));
        iommu_as[devfn]->bus_num = (uint8_t)bus_num;
        iommu_as[devfn]->devfn = (uint8_t)devfn;
        iommu_as[devfn]->iommu_state = s;

        amdvi_dev_as = iommu_as[devfn];

        /*
         * Memory region relationships looks like (Address range shows
         * only lower 32 bits to make it short in length...):
         *
         * |-----------------+-------------------+----------|
         * | Name            | Address range     | Priority |
         * |-----------------+-------------------+----------+
         * | amdvi_root      | 00000000-ffffffff |        0 |
         * |  amdvi_iommu    | 00000000-ffffffff |        1 |
         * |  amdvi_iommu_ir | fee00000-feefffff |       64 |
         * |-----------------+-------------------+----------|
         */
        memory_region_init_iommu(&amdvi_dev_as->iommu,
                                 sizeof(amdvi_dev_as->iommu),
                                 TYPE_AMD_VIOMMU_MEMORY_REGION,
                                 OBJECT(s),
                                 "amd_iommu", UINT64_MAX);
        memory_region_init(&amdvi_dev_as->root, OBJECT(s),
                           "amdvi_root", UINT64_MAX);
        address_space_init(&amdvi_dev_as->as, &amdvi_dev_as->root, name);
        memory_region_add_subregion_overlap(&amdvi_dev_as->root, 0,
                                            MEMORY_REGION(&amdvi_dev_as->iommu),
                                            1);
    }

    return &iommu_as[devfn]->as;
}

static const MemoryRegionOps mmio_mem_ops = {
    .read = amdvi_mmio_read,
    .write = amdvi_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = false,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    }
};

/*
 * TODO: We need to commnicate to VFIO/IOMMU driver to initialize the vIOMMU
 */
static void amd_viommu_init(AMDVIState *s)
{
    int i;

    s->devtab_len = 0;
    s->cmdbuf_len = 0;
    s->cmdbuf_head = 0;
    s->cmdbuf_tail = 0;
    s->evtlog_head = 0;
    s->evtlog_tail = 0;
    s->excl_enabled = false;
    s->excl_allow = false;
    s->mmio_enabled = false;
    s->enabled = false;
    s->ats_enabled = false;
    s->cmdbuf_enabled = false;

    for (i = 0; i < AMDVI_DEVID_MAX; i++)
	s->dev_domid[i] = -1;

    /* reset MMIO */
    memset(s->mmior, 0, AMD_VIOMMU_MMIO_SIZE);
    amdvi_set_quad(s, AMDVI_MMIO_EXT_FEATURES, AMDVI_DEFAULT_EXT_FEATURES,
            0xffffffffffffffef, 0);
    amdvi_set_quad(s, AMDVI_MMIO_STATUS, 0, 0x98, 0x67);

    /* reset device ident */
    pci_config_set_vendor_id(s->pci.dev.config, PCI_VENDOR_ID_AMD);
    pci_config_set_prog_interface(s->pci.dev.config, 00);
    pci_config_set_class(s->pci.dev.config, 0x0806);

    /* reset AMDVI specific capabilities, all r/o */
    pci_set_long(s->pci.dev.config + s->pci.capab_offset, AMDVI_CAPAB_FEATURES);
    pci_set_long(s->pci.dev.config + s->pci.capab_offset + AMDVI_CAPAB_BAR_LOW,
                 s->mr_mmio.addr & ~(0xffff0000));
    pci_set_long(s->pci.dev.config + s->pci.capab_offset + AMDVI_CAPAB_BAR_HIGH,
                (s->mr_mmio.addr & ~(0xffff)) >> 16);
    pci_set_long(s->pci.dev.config + s->pci.capab_offset + AMDVI_CAPAB_RANGE,
                 0xff000000);
    pci_set_long(s->pci.dev.config + s->pci.capab_offset + AMDVI_CAPAB_MISC, 0);
    pci_set_long(s->pci.dev.config + s->pci.capab_offset + AMDVI_CAPAB_MISC,
            AMDVI_MAX_PH_ADDR | AMDVI_MAX_GVA_ADDR | AMDVI_MAX_VA_ADDR);

    /*
     * For SEV-TIO guest, we need to do this after klass->launch_finish()
     */
     qemu_add_vm_change_state_handler(amd_viommu_vm_state_change, s);
}


#define FEATURE_PPR            (1ULL << 1)
#define FEATURE_XT             (1ULL << 2)
#define FEATURE_GT             (1ULL << 4)
#define FEATURE_GA             (1ULL << 7)
#define FEATURE_GIO            (1ULL << 48)
#define FEATURE_EPHSUP         (1ULL << 50)

#define FEATURE_GATS_SHIFT     12
#define FEATURE_GATS_MASK      0x03ULL
#define FEATURE_GATS_5LEVEL    ((1ULL & FEATURE_GATS_MASK) << FEATURE_GATS_SHIFT)

#define FEATURE_GLX_SHIFT      14
#define FEATURE_GLX_MASK       0x03ULL
#define FEATURE_GLX_2LEVEL     ((1ULL & FEATURE_GLX_MASK) << FEATURE_GLX_SHIFT)

#define FEATURE_PASMAX_SHIFT   32
#define FEATURE_PASMAX_MASK    0x1FULL
#define FEATURE_PASMAX_16      ((0xFULL & FEATURE_PASMAX_MASK) << FEATURE_PASMAX_SHIFT)

#define SUPPORTED_EFR	(FEATURE_PPR | FEATURE_XT | FEATURE_GT | FEATURE_GA | \
			 FEATURE_GIO | FEATURE_EPHSUP | FEATURE_GATS_5LEVEL | \
			 FEATURE_GLX_2LEVEL | FEATURE_PASMAX_16)

static int amd_viommu_mmap_mmio(AMDVIState *s)
{
    char *name;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint64_t addr = AMDVI_BASE_ADDR + (x86_iommu->index * AMD_VIOMMU_MMIO_SIZE);

fprintf(stderr, "DEBUG0: %s: out_vfmmio_mmap_offset=%#llx\n", __func__, s->iommufd_viommu_amd.out_vfmmio_mmap_offset);
    /* MMAP VF MMIO space */
    s->mmio_page3 = mmap(NULL, AMDVI_PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, s->iommufd->fd, s->iommufd_viommu_amd.out_vfmmio_mmap_offset);
    if (s->mmio_page3 == MAP_FAILED) {
	    error_report("Failed to mmap VF MMIO");
	    s->mmio_page3 = NULL;
	    return -EIO;
    }

fprintf(stderr, "DEBUG1: %s\n", __func__);
    name = g_strdup_printf("%s mmio", memory_region_name(&s->mr_mmio1));
    memory_region_init_ram_device_ptr(&s->mr_mmio1, memory_region_owner(&s->mr_mmio1),
                                      name, 0x1000, s->mmio_page3);
    memory_region_add_subregion_overlap(get_system_memory(), addr + 0x2000,
                                        &s->mr_mmio1, 1);
    g_free(name);
fprintf(stderr, "DEBUG2: %s\n", __func__);

    return 0;
}

static void amdvi_alloc_vdev(PCIBus *b, PCIDevice *d, void *opaque)
{
    AMDIOMMUFDDevice *amd_idev = (AMDIOMMUFDDevice *)opaque;
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    uint16_t rid = pci_requester_id(b->parent_dev);
    struct AMDVI_dte_key key = {
        .bus = b,
        .devfn = d->devfn,
    };

    if (!g_hash_table_lookup(amd_idev->iommu_state->hiod_hash, &key))
        return;

    if (object_dynamic_cast(OBJECT(b->parent_dev), TYPE_PCI_BRIDGE))
        rid += 0x100;

    fprintf(stderr, "DEBUG: %s: --- bus=%#x, bus_parent_dev=%s, device name=%s, rid=%#x\n", __func__,
            pci_bus_num(b), b->parent_dev->name, d->name, rid);

    amd_idev->core = iommufd_backend_alloc_vdev(idev, amd_idev->iommu_state->core, rid);
    if (!amd_idev->core) {
        error_report("failed to allocate a vDEVICE");
    }
}

static void *amdvi_walk_bus(PCIBus *b, void *opaque)
{
    uint16_t rid = pci_requester_id(b->parent_dev);

    fprintf(stderr, "DEBUG: %s: bus_name=%s, bus=%#x, bus_parent_dev=%s, rid=%#x\n", __func__,
    b->qbus.name, pci_bus_num(b), b->parent_dev->name, rid);

    pci_for_each_device_under_bus(b, amdvi_alloc_vdev, opaque);
    return opaque;
}

static int amdvi_viommu_initialized_one(AMDVIState *s, AMDIOMMUFDDevice *amd_idev)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    VFIODevice *vbasedev = amd_idev->hiod->agent;
    VFIOPCIDevice *vdev = container_of(vbasedev, VFIOPCIDevice, vbasedev);
    PCIDevice *pdev = &vdev->pdev;
    int viommu_devid = pci_bus_num(pci_get_bus(pdev)) | pdev->devfn;
    uint16_t bdf = PCI_BUILD_BDF((s->iommu.host.bus),
                                 PCI_DEVFN(s->iommu.host.slot,
                                 s->iommu.host.function));

    s->iommufd_viommu_amd.iommu_devid = bdf;
    s->iommufd_viommu_amd.viommu_devid = viommu_devid;
    s->iommufd_viommu_amd.trans_devid = s->translate_id;

    s->core = iommufd_backend_alloc_viommu(s->iommufd,
                                           idev->devid,
                                           IOMMU_VIOMMU_TYPE_AMD,
                                           amd_idev->v1_hwpt.hwpt_id,
                                           sizeof(s->iommufd_viommu_amd),
                                           &s->iommufd_viommu_amd);
    if (!s->core) {
        error_report("failed to allocate a viommu");
        return -EINVAL;
    }

    s->gid = s->iommufd_viommu_amd.out_gid;
    s->enabled = true;
    fprintf(stderr, "DEBUG: %s: iommufd vIOMMU initialied. gid=%#x, viommu_devid=%#x\n",
            __func__, s->gid, viommu_devid);
    return 0;
}

/*
 * Note: This must be called when running state is RUN_STATE_RUNNING
 */
static void amdvi_vdevice_viommu_setup(AMDVIState *s, AMDIOMMUFDDevice *amd_idev)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

    if (amd_viommu_get_v1_hwpt(amd_idev, idev->devid, s))
        return;

    if (!s->enabled && amdvi_viommu_initialized_one(s, amd_idev))
        return;

    if (amd_viommu_mmap_mmio(s))
        goto out_hwpt;

    pci_for_each_bus_depth_first(s->primary_bus, amdvi_walk_bus, NULL, amd_idev);

    return;

out_hwpt:
    /* TODO: Detach hwpt and free hwpt id */
    return;
}

static void _build_efr_guest_translation(HostIOMMUDeviceHwInfo *hwinfo,
                                         uint64_t *efr, uint64_t  *efr2)
{
    *efr = (hwinfo->amd.efr & SUPPORTED_EFR);
    *efr2 = 0ULL;
}


static bool amdvi_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                   HostIOMMUDevice *hiod, Error **errp)
{
    AMDVIState *s = opaque;
    VFIODevice *vbasedev = hiod->agent;
    VFIOPCIDevice *vdev = container_of(vbasedev, VFIOPCIDevice, vbasedev);
    PCIDevice *pdev = &vdev->pdev;
    AMDIOMMUFDDevice *amd_idev;
    struct AMDVI_dte_key *new_key;
    struct AMDVI_dte_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    HostIOMMUDeviceHwInfo hwinfo = hiod->hwinfo;

    assert(hiod);
    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    fprintf(stderr, "DEBUG: %s: bus=%s(%#x), devfn=%#x, pdev_name=%s\n", __func__,
	    bus->qbus.name, pci_bus_num(bus), devfn, pdev->name);

    /* TODO: Replace this w/ amd_iommufd_dev_hash? */
    if (g_hash_table_lookup(s->hiod_hash, &key)) {
        error_setg(errp, "Host IOMMU device already exist");
        return false;
    }

    if (hiod->caps.type != IOMMU_HW_INFO_TYPE_AMD) {
        error_report("IOMMU hardware type %#x is not compatible!!!", hiod->caps.type);
        return false;
    }

    new_key = g_malloc(sizeof(*new_key));
    new_key->bus = bus;
    new_key->devfn = devfn;

    object_ref(hiod);
    g_hash_table_insert(s->hiod_hash, new_key, hiod);

    amd_idev = g_malloc0(sizeof(AMDIOMMUFDDevice));
    amd_idev->iommu_state = s;
    amd_idev->hiod = hiod;

    g_hash_table_insert(s->amd_iommufd_dev_hash, new_key, amd_idev);

    /* Use iommufd handler opened by device */
    s->iommufd = idev->iommufd;
    _build_efr_guest_translation(&hwinfo, (uint64_t*) &s->hwinfo.efr,
                                 (uint64_t*) &s->hwinfo.efr2);

    fprintf(stderr, "DEBUG %s: hwinfo 0x%llx 0x%llx\n",
            __func__, s->hwinfo.efr, s->hwinfo.efr2);

    return true;
}

static void amdvi_unset_iommu_device(PCIBus *bus, void *opaque,
                                     int devfn)
{
    AMDVIState *s = opaque;
    struct AMDVI_dte_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    int devid = (pci_bus_num(bus) << 8) | devfn;
    AMDIOMMUFDDevice *dev;

    if (!g_hash_table_lookup(s->hiod_hash, &key)) {
        return;
    }

    g_hash_table_remove(s->hiod_hash, &key);

    dev = amd_viommu_get_device_from_bdf(s, devid >> 8, devid & 0xFF);
    if (!dev) {
        fprintf(stderr, "DEBUG: %s: %u: Failed get_device_from_bdf\n", __func__, __LINE__);
        return;
    }

    amd_viommu_put_v1_hwpt(dev, devid, s);
}

static void amd_viommu_state_change_running(AMDVIState *s)
{
    struct amd_as_key *key;
    AMDIOMMUFDDevice *amd_idev;
    GHashTableIter it;

    g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

    /* Go through each VFIO device */
    while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
        amdvi_vdevice_viommu_setup(s, amd_idev);
    }
}

static void amd_viommu_vm_state_change(void *opaque,
                                        bool running, RunState state)
{
    AMDVIState *s = opaque;

    fprintf(stderr, "DEBUG: %s: state=%u\n", __func__, state);

    switch (state) {
    case RUN_STATE_RUNNING:
        amd_viommu_state_change_running(s);
        break;
    case RUN_STATE_SHUTDOWN:
    default:
        break;
    }
}

static PCIIOMMUOps amdvi_iommu_ops = {
    .set_iommu_device = amdvi_set_iommu_device,
    .unset_iommu_device = amdvi_unset_iommu_device,
    .get_address_space = NULL,
};

static gboolean amd_as_equal(gconstpointer v1, gconstpointer v2)
{
    const struct amd_as_key *key1 = v1;
    const struct amd_as_key *key2 = v2;

    return (key1->bus == key2->bus) && (key1->devfn == key2->devfn) &&
           (key1->pasid == key2->pasid);
}

/*
 * Note that we use pointer to PCIBus as the key, so hashing/shifting
 * based on the pointer value is intended. Note that we deal with
 * collisions through amd_as_equal().
 */
static guint amd_as_hash(gconstpointer v)
{
    const struct amd_as_key *key = v;
    guint value = (guint)(uintptr_t)key->bus;

    return (guint)(value << 8 | key->devfn);
}

static void amd_viommu_unrealize(DeviceState *dev)
{
    AMDVIState *s = AMD_VIOMMU_DEVICE(dev);

    if (s->hwpt_cnt == 0)
        munmap(s->mmio_page3, AMDVI_PAGE_SIZE);
}

static guint amdvi_dte_hash(gconstpointer v)
{
    const struct AMDVI_dte_key *key = v;
    guint value = (guint)(uintptr_t)key->bus;

    return (guint)(value << 8 | key->devfn);
}

static gboolean amdvi_dte_equal(gconstpointer v1, gconstpointer v2)
{
    const struct AMDVI_dte_key *key1 = v1;
    const struct AMDVI_dte_key *key2 = v2;

    return (key1->bus == key2->bus) && (key1->devfn == key2->devfn);
}

static void amd_viommu_realize(DeviceState *dev, Error **errp)
{
    int ret = 0;
    AMDVIState *s = AMD_VIOMMU_DEVICE(dev);
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(dev);
    MachineState *ms = MACHINE(qdev_get_machine());
    PCMachineState *pcms = PC_MACHINE(ms);
    PCIBus *bus = pcms->pcibus;

    /* This device should take care of IOMMU PCI properties */
    qdev_set_parent_bus(DEVICE(&s->pci), &bus->qbus, &error_abort);
    object_property_set_bool(OBJECT(&s->pci), "realized", true, errp);
    ret = pci_add_capability(&s->pci.dev, AMDVI_CAPAB_ID_SEC, 0,
                                         AMDVI_CAPAB_SIZE, errp);
    if (ret < 0) {
        return;
    }
    s->pci.capab_offset = ret;

    ret = pci_add_capability(&s->pci.dev, PCI_CAP_ID_MSI, 0,
                             AMDVI_CAPAB_REG_SIZE, errp);
    if (ret < 0) {
        return;
    }
    ret = pci_add_capability(&s->pci.dev, PCI_CAP_ID_HT, 0,
                             AMDVI_CAPAB_REG_SIZE, errp);
    if (ret < 0) {
        return;
    }

    s->devtab = g_malloc0(AMDVI_DEVTAB_SIZE);
    memset(s->devtab, 0, AMDVI_DEVTAB_SIZE);

    /* setup IOMMU PCI device ID in the guest. */
    amd_viommu_host_dma_iommu(bus, s, s->pci.dev.devfn);

    /* set up MMIO */
    memory_region_init_io(&s->mr_mmio, OBJECT(s), &mmio_mem_ops, s, "amdvi-mmio",
                          AMD_VIOMMU_MMIO_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mr_mmio);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, AMDVI_BASE_ADDR + (x86_iommu->index * AMD_VIOMMU_MMIO_SIZE));

    /*
     * Setup vIOMMU for the device w/o specifying the iommu_fn to avoid
     * calling amdvi_host_dma_iommu(). See pci_device_iommu_addres_space()
     * on calling of iommu_fn().
     */
    if (s->primary_bus)
       bus = s->primary_bus;
    pci_setup_iommu(bus, &amdvi_iommu_ops, s);

    msi_init(&s->pci.dev, 0, 1, true, false, errp);

    amd_viommu_init(s);

    s->amd_iommufd_dev_hash = g_hash_table_new_full(amd_as_hash, amd_as_equal,
                                      g_free, g_free);

    s->hiod_hash = g_hash_table_new_full(amdvi_dte_hash,
                                         amdvi_dte_equal, g_free, g_free);
}

static const VMStateDescription vmstate_amdvi = {
    .name = "amd-viommu",
    .unmigratable = 1
};

static void amd_viommu_instance_init(Object *obj)
{
    AMDVIState *s = AMD_VIOMMU_DEVICE(obj);

    object_initialize(&s->pci, sizeof(s->pci), TYPE_AMD_VIOMMU_PCI);
}

static const Property amd_viommu_properties[] = {
    DEFINE_PROP_LINK("iommufd", AMDVIState, iommufd,
                     TYPE_IOMMUFD_BACKEND, IOMMUFDBackend *),
    DEFINE_PROP_LINK("primary-bus", AMDVIState, primary_bus,
                     TYPE_PCIE_BUS, PCIBus *),
    DEFINE_PROP_UINT32("last-bus-nr", AMDVIState, last_bus_nr, 0),
    DEFINE_PROP_UINT32("translate-id", AMDVIState, translate_id, 0),
};

static void amd_viommu_class_init(ObjectClass *klass, void* data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *dc_class = X86_IOMMU_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_amdvi;
    device_class_set_props(dc, amd_viommu_properties);
    dc->hotpluggable = false;
    dc_class->realize = amd_viommu_realize;
    dc_class->unrealize = amd_viommu_unrealize;

    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD VIOMMU device";
}

static const TypeInfo AmdViommu = {
    .name = TYPE_AMD_VIOMMU_DEVICE,
    .parent = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(AMDVIState),
    .instance_init = amd_viommu_instance_init,
    .class_init = amd_viommu_class_init
};

static const TypeInfo AmdViommuPCI = {
    .name = "AMD-VIOMMU-PCI",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDVIPCIState),
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const TypeInfo amd_viommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_AMD_VIOMMU_MEMORY_REGION,
};

static void amd_viommu_pci_register_types(void)
{
    type_register_static(&AmdViommuPCI);
    type_register_static(&AmdViommu);
    type_register_static(&amd_viommu_memory_region_info);
}

type_init(amd_viommu_pci_register_types);
