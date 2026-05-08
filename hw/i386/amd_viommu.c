#include "qemu/osdep.h"

#include <sys/ioctl.h>
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/i386/pc.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "trace.h"

#include "amd_iommu.h"
#include "amd_viommu.h"

static void amdvi_set_quad(AMDVIState *s, hwaddr addr, uint64_t val,
                           uint64_t romask, uint64_t w1cmask)
{
    stq_le_p(&s->mmior[addr], val);
    stq_le_p(&s->romask[addr], romask);
    stq_le_p(&s->w1cmask[addr], w1cmask);
}

static uint64_t amdvi_readq(AMDVIState *s, hwaddr addr)
{
    return ldq_le_p(&s->mmior[addr]);
}

/* external write */
static void amdvi_writew(AMDVIState *s, hwaddr addr, uint16_t val)
{
    uint16_t romask = lduw_le_p(&s->romask[addr]);
    uint16_t w1cmask = lduw_le_p(&s->w1cmask[addr]);
    uint16_t oldval = lduw_le_p(&s->mmior[addr]);
    stw_le_p(&s->mmior[addr],
            ((oldval & romask) | (val & ~romask)) & ~(val & w1cmask));
}

static void amdvi_writel(AMDVIState *s, hwaddr addr, uint32_t val)
{
    uint32_t romask = ldl_le_p(&s->romask[addr]);
    uint32_t w1cmask = ldl_le_p(&s->w1cmask[addr]);
    uint32_t oldval = ldl_le_p(&s->mmior[addr]);
    stl_le_p(&s->mmior[addr],
            ((oldval & romask) | (val & ~romask)) & ~(val & w1cmask));
}

static void amdvi_writeq(AMDVIState *s, hwaddr addr, uint64_t val)
{
    uint64_t romask = ldq_le_p(&s->romask[addr]);
    uint64_t w1cmask = ldq_le_p(&s->w1cmask[addr]);
    uint32_t oldval = ldq_le_p(&s->mmior[addr]);
    stq_le_p(&s->mmior[addr],
            ((oldval & romask) | (val & ~romask)) & ~(val & w1cmask));
}

/* OR a 64-bit register with a 64-bit value storing result in the register */
static int amd_viommu_mmio_write(AMDVIState *s, __u32 offset,
                                 __u32 size, __u64 value)
{
    struct iommu_viommu_command arg = {
        .size = sizeof(arg),
        .object_id = s->core->viommu_id,
        .op = IOMMU_VIOMMU_COMMAND_OP_SET,
        .index = offset,
        .val64 = value,
    };

    return ioctl(s->iommufd->fd, IOMMU_VIOMMU_COMMAND, &arg);
}

static int amd_viommu_mmio_read(AMDVIState *s, __u32 offset,
                                __u32 size, __u64 *value)
{
    int ret;
    struct iommu_viommu_command arg = {
        .size = sizeof(arg),
        .object_id = s->core->viommu_id,
        .op = IOMMU_VIOMMU_COMMAND_OP_GET,
        .index = offset,
        .val64 = 0,
    };

    ret = ioctl(s->iommufd->fd, IOMMU_VIOMMU_COMMAND, &arg);
    if (ret) {
        return ret;
    }

    *value = arg.val64;

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

struct amd_as_key {
    PCIBus *bus;
    uint8_t devfn;
    uint32_t pasid;
};

static AMDIOMMUFDDevice *amd_viommu_get_device_from_bdf(AMDVIState *s, uint16_t bus, uint16_t devfn)
{
    AMDIOMMUFDDevice *amd_idev;
    struct amd_as_key *key;
    GHashTableIter as_it;

    g_hash_table_iter_init(&as_it, s->amd_iommufd_dev_hash);

fprintf(stderr, "DEBUG0: %s: bus=%#x, devfn=%#x\n", __func__, bus, devfn);

    while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
fprintf(stderr, "DEBUG1: %s: iter bus=%#x, devfn=%#x\n", __func__, pci_bus_num(key->bus), key->devfn);
        if (pci_bus_num(key->bus) == bus && key->devfn == devfn)
            return amd_idev;
    }
    return NULL;
}

static uint64_t get_gcr3_trp(uint64_t *dte)
{
    uint64_t tmp1, tmp2, tmp3;

    tmp1 = (dte[0] >> 58) & 0x7ULL;
    tmp2 = (dte[1] >> 16) & 0xFFFFULL;
    tmp3 = (dte[1] >> 43) & 0x3FFFFFULL;

    return (tmp1 << 12) | (tmp2 << 15) | (tmp3 << 31);
}

static int amd_viommu_update_gcr3(AMDVIState *s, AMDIOMMUFDDevice *dev,
				  uint16_t dev_id, uint64_t *dte)
{
    int ret;
    uint32_t hwpt_id, old_hwpt;
    Error *local_err = NULL;
    struct iommu_hwpt_amd_guest hwpt;
    VFIODevice *vdev = dev->hiod->agent;

    hwpt.dte[0] = dte[0];
    hwpt.dte[1] = dte[1];
    hwpt.dte[2] = dte[2];
    hwpt.dte[3] = dte[3];

    fprintf(stderr, "DEBUG: %s: ALLOC , gdevid=%#x, dte=%016lx:%016lx:%016lx:%016lx\n",
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
                                     sizeof(hwpt),
                                     &hwpt,
                                     &hwpt_id,
                                     &local_err);
    if (!ret)
        return -EINVAL;


    old_hwpt = dev->v2_hwpt_id;
    dev->v2_hwpt_id = hwpt_id;

    fprintf(stderr, "DEBUG: %s: ATTACH, idev.dev_id=%#x, hwpt_id=%#x:%#x\n",
	    __func__, vdev->idev.dev_id, dev->v1_hwpt.hwpt_id, hwpt_id);

    ret = iommufd_device_attach_hwpt(&vdev->idev, hwpt_id);

    if (old_hwpt) {
        iommufd_backend_free_id(s->iommufd, old_hwpt);
    }

    return ret;
}

static void amd_viommu_dte_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *dev;
    AMDVI_dte_info *dte_info;
    bool v = false;
    uint64_t gcr3_trp;
    uint64_t dte[4];
    uint64_t offset0 = 0, offset1 = 0, offset2 = 0, offset3 = 0;
    uint32_t devid;

    /* Storing value in the devtab */
    if (size ==  2) {
        stw_le_p(&s->devtab[offset], val);
    } else if (size == 4) {
        stl_le_p(&s->devtab[offset], val);
    } else if (size == 8) {
        stq_le_p(&s->devtab[offset], val);
    }

    /*
     * Calculate devid from offset using DTE size 0x20. Note that this is offset
     * from the devid from primary bus. See amd_viommu_handle_dev_tab_mmio_write().
     */
    devid = (pci_bus_num(s->root_bus) << 8) + (offset >> 5);

//    fprintf(stderr, "DEBUG: %s: primary_bus=%#x, devid=%#x, offset=%#lx \n",
//	__func__, pci_bus_num(s->primary_bus), devid, offset);

    if (devid > ((s->last_bus_nr << 8) | 0xFF)) {
        error_printf("%s: devid %#x is out of range (%#x)\n", __func__,
                     devid, ((s->last_bus_nr << 8) | 0xFF));
        return;
    }

    dte_info = &s->dte_info[devid];
    dte_info->last = dte_info->curr;
    dte_info->curr = (offset % 0x20) >> 3;

    /*
     * Note:
     * For now, we only care about the case when writing to
     * DTE[1] (for DomainID, GCR3 Table Root Pointer)
     * DTE[2] (for GuestPagingMode).
     *
     * FIXME: Needs to handle 128-bit
     */
    if (offset % 0x20 == 0) {
	offset3 = offset + 0x18;
	offset2 = offset + 0x10;
	offset1 = offset + 0x8;
	offset0 = offset;
    } else if (offset % 0x20 == 0x8) {
	offset3 = offset + 0x10;
	offset2 = offset + 0x8;
	offset1 = offset;
	offset0 = offset - 0x8;
    } else if (offset % 0x20 == 0x10) {
	offset3 = offset + 0x8;
	offset2 = offset;
	offset1 = offset - 0x8;
	offset0 = offset - 0x10;
    } else if (offset % 0x20 == 0x18) {
	offset3 = offset;
	offset2 = offset - 0x8;
	offset1 = offset - 0x10;
	offset0 = offset - 0x18;
    }

    dte[0] = amd_viommu_dte_read(opaque, offset0, size);
    dte[1] = amd_viommu_dte_read(opaque, offset1, size);
    dte[2] = amd_viommu_dte_read(opaque, offset2, size);
    dte[3] = amd_viommu_dte_read(opaque, offset3, size);

    v = dte[0] & 0x1ULL;
    gcr3_trp = get_gcr3_trp(dte);

    fprintf(stderr, "DEBUG: %s: gdevid=%#04x, gcr3_trp=%016lx DTE[%lu] offset=%#05lx, dte=%016lx:%016lx:%016lx:%016lx\n",
            __func__, devid, gcr3_trp, (offset % 0x20) >> 3, offset, dte[0], dte[1], dte[2], dte[3]);

    /*
     * Handle cases:
     * 1. Writing DTE[1] then DTE[0]
     * 2. Writing DTE[0] then DTE[1] since
     *    - Linux 6.12 and older does not guarantee the ordering.
     *    - Linux 6.13 and later uses cmpxchg16, which starts from
     *      least significant bit.
     */
    if ((dte_info->last == 1 && dte_info->curr == 0) ||
        (dte_info->last == 0 && dte_info->curr == 1)) {
        /*
         * Update GCR3 when V and GV is set, and GCR3 is updated.
         */
        if (!v || (dte_info->dte0 == dte[0] && dte_info->dte1 == dte[1]))
            return;

        int ret;

        /* FIXME: Currently, we iterate through all devices instead of g_hash_table_lookup()
         * since we cannot get PCIBus from bus number to use it as key
         *
         * TODO: Use bus number itsecd4482c6017elf in key as bus number is used in comparision
         */
        dev = amd_viommu_get_device_from_bdf(s, devid >> 8, devid & 0xFF);
        if (!dev) {
            fprintf(stderr, "DEBUG: %s: %u: Failed get_device_from_bdf (%#x)\n",
                    __func__, __LINE__, devid);
            exit(-EINVAL);
        }

        /* Write everything */
        ret = amd_viommu_update_gcr3(s, dev, devid, dte);
        if (ret)
            return;

        dte_info->dte0 = dte[0];
        dte_info->dte1 = dte[1];

        /* clear dte tracking */
	dte_info->last = -1;
	dte_info->curr = -1;
    }
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

static void cleanup_devtab(AMDVIState *s)
{
    memory_region_del_subregion(get_system_memory(), &s->devtab_mr);
    object_unparent(OBJECT(&s->devtab_mr));
    free(s->devtab);
    s->devtab = NULL;
    s->devtab_base = 0;
    s->devtab_len = 0;
    s->devtab_size = 0;
}

static inline void amdvi_handle_devtab_mmio_write(AMDVIState *s)
{
    char name[30];
    uint64_t offset, req_size;
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_DEVICE_TABLE);

    snprintf(name, 30, "%s-%02u", "amd-iommu-devtab", s->iommu.index);

    /* Free up previously allocated table */
    if (s->devtab) {
        cleanup_devtab(s);
    }

    s->devtab_base = (val & AMDVI_MMIO_DEVTAB_BASE_MASK);

    /* Offset to the first entry in DTE to be set up for listener */
    offset = s->devtab_base + ((pci_bus_num(s->root_bus) << 8) * 0x20);

    /* The devtab_size is the size specifyied by guest indicated is (n + 1) * 4 Kbytes. */
    s->devtab_size = ((val & AMDVI_MMIO_DEVTAB_SIZE_MASK) + 1) << 12;

    /*
     * The devtab_len is calculated based on the reported bus information
     * for this IOMMU
     */
    s->devtab_len = ((s->last_bus_nr - pci_bus_num(s->root_bus) + 1) << 8);
    s->devtab_len *= 0x20;

    /* The size is calculated from the space before the bus + devtab_len */
    req_size = ((pci_bus_num(s->root_bus) << 8) * 0x20) + s->devtab_len;
    if (req_size > s->devtab_size) {
         fprintf(stderr, "%s: Invalid devtab size %#lx (required %#lx)\n", __func__,
                 s->devtab_size, req_size);
         exit(-EINVAL);
    }

    /* Only allocate devtab storage for the specified bus range */
    s->devtab = g_malloc0(s->devtab_len);
    if (!s->devtab)
        exit(-ENOMEM);

    memset(s->devtab, 0, s->devtab_len);

    fprintf(stderr, "DEBUG: %s: %s, base=%#lx, offset=%#lx, bus=%#x, last_bus=%#x, len=%#lx size=%#lx, req_size=%#lx\n",
            __func__, name, s->devtab_base, offset,
            pci_bus_num(s->root_bus), s->last_bus_nr, s->devtab_len,
            s->devtab_size, req_size);

    /*
     * Set up memory listener for IOMMU Device Table for DTEs within
     * the range of devid for this IOMMU only
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
        error_report("%s: failed to allocate command buffer\n", __func__);
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
        error_report("%s: failed to allocate event log\n", __func__);
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
        error_report("%s: failed to allocate ppr log\n", __func__);
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

    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amdvi_mmio_write("error: addr outside region: max ",
                (uint64_t)AMDVI_MMIO_SIZE, size, val, offset);
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

static uint64_t amdvi_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AMDVIState *s = opaque;
    uint64_t val = -1;
    int ret;

    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amdvi_mmio_read_invalid(AMDVI_MMIO_SIZE, addr, size);
        return (uint64_t)-1;
    }

    ret = amd_viommu_mmio_read(s, (addr & ~0x07), size, (__u64 *)&val);
    if (ret)
        val = -1;

    return val;
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
 * TODO: add command line arguments
 * TODO: This patch is big so better to split it
 *  --- Introducing address spaces
 *  --- Updating header with SUPPORTED_EFR
 *  --- Implementing set/unset iommu device
 */
static void _build_efr_guest_translation(VendorCaps *caps,
                                         uint64_t *efr, uint64_t  *efr2)
{
    *efr = (caps->amd.efr & SUPPORTED_EFR);
    *efr2 = 0ULL;
}

struct AMDVI_dte_key {
    PCIBus *bus;
    uint8_t devfn;
};

static bool amd_viommu_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                   HostIOMMUDevice *hiod, Error **errp)
{
    AMDVIState *s = opaque;
    VFIODevice *vbasedev = hiod->agent;
    VFIOPCIDevice *vdev = container_of(vbasedev, VFIOPCIDevice, vbasedev);
    PCIDevice *pdev = &vdev->parent_obj;
    AMDIOMMUFDDevice *amd_idev;
    struct AMDVI_dte_key *new_key;
    struct AMDVI_dte_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    VendorCaps *caps = &hiod->caps.vendor_caps;

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

    g_hash_table_insert(s->hiod_hash, new_key, hiod);

    amd_idev = g_malloc0(sizeof(AMDIOMMUFDDevice));
    amd_idev->iommu_state = s;
    amd_idev->hiod = hiod;
    amd_idev->v2_hwpt_id =  0;
    amd_idev->passthrough_hwpt_id = 0;

    g_hash_table_insert(s->amd_iommufd_dev_hash, new_key, amd_idev);

    /* Use iommufd handler opened by device */
    s->iommufd = idev->iommufd;
    _build_efr_guest_translation(caps, (uint64_t*) &s->hwinfo.efr,
                                 (uint64_t*) &s->hwinfo.efr2);

    fprintf(stderr, "DEBUG %s: hwinfo 0x%llx 0x%llx\n",
            __func__, s->hwinfo.efr, s->hwinfo.efr2);

    return true;
}

static void amd_viommu_unset_iommu_device(PCIBus *bus, void *opaque,
                                     int devfn)
{
    AMDVIState *s = opaque;
    struct AMDVI_dte_key key = {
        .bus = bus,
        .devfn = devfn,
    };

    if (!g_hash_table_lookup(s->hiod_hash, &key)) {
        return;
    }

    g_hash_table_remove(s->hiod_hash, &key);
}

static AddressSpace *amd_viommu_get_address_space(PCIBus *bus, void *opaque, int devfn)
{
    /* TODO: raise error if we find a non-iommufd device */
    return &address_space_memory;
}

static int amd_viommu_get_x86_iommu(void *opaque, void **x86_iommu)
{
    AMDVIState *s = opaque;
    *x86_iommu = X86_IOMMU_DEVICE(s);
    return 0;
}

static const PCIIOMMUOps amdvi_iommu_ops = {
    .get_address_space = amd_viommu_get_address_space,
    .set_iommu_device = amd_viommu_set_iommu_device,
    .unset_iommu_device = amd_viommu_unset_iommu_device,
    .get_x86_iommu = amd_viommu_get_x86_iommu,
};

static void amdvi_pci_realize(PCIDevice *pdev, Error **errp)
{
    AMDVIPCIState *s = AMD_VIOMMU_PCI(pdev);
    int ret;

    ret = pci_add_capability(pdev, AMDVI_CAPAB_ID_SEC, 0,
                             AMDVI_CAPAB_SIZE, errp);
    if (ret < 0) {
        return;
    }
    s->capab_offset = ret;

    ret = pci_add_capability(pdev, PCI_CAP_ID_MSI, 0,
                             AMDVI_CAPAB_REG_SIZE, errp);
    if (ret < 0) {
        return;
    }
    ret = pci_add_capability(pdev, PCI_CAP_ID_HT, 0,
                             AMDVI_CAPAB_REG_SIZE, errp);
    if (ret < 0) {
        return;
    }

    /* TODO : do we need this */
    if (msi_init(pdev, 0, 1, true, false, errp) < 0) {
        return;
    }

    /* reset device ident */
    pci_config_set_prog_interface(pdev->config, 0);

    /* reset AMDVI specific capabilities, all r/o */
    pci_set_long(pdev->config + s->capab_offset, AMD_VIOMMU_CAPAB_FEATURES);
    pci_set_long(pdev->config + s->capab_offset + AMDVI_CAPAB_RANGE,
                 0xff000000);
    pci_set_long(pdev->config + s->capab_offset + AMDVI_CAPAB_MISC, 0);
    pci_set_long(pdev->config + s->capab_offset + AMDVI_CAPAB_MISC,
            AMDVI_MAX_PH_ADDR | AMDVI_MAX_GVA_ADDR | AMDVI_MAX_VA_ADDR);
}

static const VMStateDescription vmstate_amd_viommu = {
    .name = "amd-viommu",
    .unmigratable = 1
};

static PCIBus *get_root_bus(struct PCIBus *bus)
{
    while (bus) {

        if (!pci_bus_is_express(bus)) {
            error_report("attached to non PCI express bus %s",
                         bus->qbus.name);
            goto err_out;
        }

        if (bus->iommu_ops) {
            error_report("different IOMMU already serves bus %s",
                         bus->qbus.name);
            goto err_out;
        }

        if (pci_bus_is_root(bus) &&
            object_dynamic_cast(OBJECT(bus)->parent, TYPE_PCI_HOST_BRIDGE)) {

            if (bus->parent_dev &&
                !object_dynamic_cast(OBJECT(bus), TYPE_PXB_PCIE_BUS)) {

                    error_report("only supports PXB-PCI as extra root bus");
                    goto err_out;
            }
            /* We found the valid root bus so return it to the user*/
            goto out;
        }

        if (bus->parent_dev) {
            bus = pci_get_bus(bus->parent_dev);
        } else {
            error_report("could not find valid PCIE bus");
            goto err_out;
        }
    };

err_out:
    exit(EXIT_FAILURE);
out:
    return bus;
}

static void amdvi_init(AMDVIState *s)
{

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
    s->cmdbuf_enabled = false;

    /* reset MMIO */
    memset(s->mmior, 0, AMDVI_MMIO_SIZE);
    amdvi_set_quad(s, AMDVI_MMIO_EXT_FEATURES, AMD_VIOMMU_DEFAULT_EXT_FEATURES,
            0xffffffffffffffef, 0);
    amdvi_set_quad(s, AMDVI_MMIO_STATUS, 0, 0x98, 0x67);

    for (int i = 0; i < AMDVI_DEVID_MAX; i++) {
	    s->dte_info[i].last = -1;
	    s->dte_info[i].curr = -1;
	    s->dte_info[i].dte0 = ~0ULL;
	    s->dte_info[i].dte1 = ~0ULL;
    }
}


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

static void amd_viommu_sysbus_realize(DeviceState *dev, Error **errp)
{
    AMDVIState *s = AMD_VIOMMU_DEVICE(dev);
    MachineState *ms = MACHINE(qdev_get_machine());
    PCMachineState *pcms = PC_MACHINE(ms);
    X86MachineState *x86ms = X86_MACHINE(ms);
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    PCIBus *iommu_bus;
    uint64_t base_addr;

    if (s->pci_id) {
        PCIDevice *pdev = NULL;
        int ret = pci_qdev_find_device(s->pci_id, &pdev);

        if (ret) {
            error_report("Cannot find PCI device '%s'", s->pci_id);
            return;
        }

        if (!object_dynamic_cast(OBJECT(pdev), TYPE_AMD_VIOMMU_PCI)) {
            error_report("Device '%s' must be an AMDVI-PCI device type", s->pci_id);
            return;
        }

        iommu_bus = pci_get_bus(pdev);
        s->pci = AMD_VIOMMU_PCI(pdev);
    } else {
        iommu_bus = pcms->pcibus;
        s->pci = AMD_VIOMMU_PCI(object_new(TYPE_AMD_VIOMMU_PCI));
        /* This device should take care of IOMMU PCI properties */
        if (!qdev_realize(DEVICE(s->pci), &iommu_bus->qbus, errp)) {
            return;
        }
    }

    base_addr = AMDVI_GET_BASE_ADDR(x86_iommu->index);

    s->amd_iommufd_dev_hash = g_hash_table_new_full(amd_as_hash, amd_as_equal,
                                      g_free, g_free);

    s->hiod_hash = g_hash_table_new_full(amdvi_dte_hash,
                                         amdvi_dte_equal,
                                         g_free,
                                         g_free);

    /* Set up PCI Capability base address */
    pci_set_long(s->pci->dev.config + s->pci->capab_offset + AMDVI_CAPAB_BAR_LOW,
                 base_addr & MAKE_64BIT_MASK(14, 18));
    pci_set_long(s->pci->dev.config + s->pci->capab_offset + AMDVI_CAPAB_BAR_HIGH,
                base_addr >> 32);

    s->root_bus = get_root_bus(iommu_bus);

    /* set up MMIO */
    /* EXPLANATION INTERNAL: Keep the the AMDVI_MMIO_SIZE as it is, do not
     * crop it to 0x2000 as we use priority 1 for the vfmmio
     */
    memory_region_init_io(&s->mr_mmio, OBJECT(s), &mmio_mem_ops, s,
                          "amdvi-mmio", AMDVI_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), base_addr,
                                &s->mr_mmio);

    /**
     * Pseudo address space under root PCI bus.
     * The linux kernel disables the intremap when it cannot find IOAPIC under
     * AMD IOMMU IVRS, hence create a ioapic_as with root bus even though AMD
     * IOMMU is not serving devices attached to root bus.
     */
    if (!x86ms->ioapic_iommu && x86_iommu_ir_supported(X86_IOMMU_DEVICE(s))) {
        x86ms->ioapic_iommu = x86_iommu;
    }

    pci_setup_iommu(s->root_bus, &amdvi_iommu_ops, s);
    amdvi_init(s);
}

static void amd_viommu_sysbus_reset(DeviceState *dev)
{
    AMDVIState *s = AMD_VIOMMU_DEVICE(dev);
    if (s->devtab) {
        cleanup_devtab(s);
    }
}

static const Property amd_viommu_properties[] = {
    DEFINE_PROP_STRING("pci-id", AMDVIState, pci_id),
    DEFINE_PROP_UINT32("last-bus-nr", AMDVIState, last_bus_nr, 0),
};

static void amd_viommu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *dc_class = X86_IOMMU_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, amd_viommu_sysbus_reset);
    dc->vmsd = &vmstate_amd_viommu;
    dc->hotpluggable = false;
    dc_class->realize = amd_viommu_sysbus_realize;

    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD VIOMMU device";
    device_class_set_props(dc, amd_viommu_properties);
}

static const TypeInfo AmdViommu = {
    .name = TYPE_AMD_VIOMMU_DEVICE,
    .parent = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(AMDVIState),
    .class_init = amd_viommu_class_init
};

static void amd_viommu_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = 0x1419;
    k->class_id = 0x0806;
    k->realize = amdvi_pci_realize;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD IOMMU (AMD-Vi) DMA Remapping device";
}

static const TypeInfo AmdViommuPCI = {
    .name = "AMD-VIOMMU-PCI",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDVIPCIState),
    .class_init = amd_viommu_pci_class_init,
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
