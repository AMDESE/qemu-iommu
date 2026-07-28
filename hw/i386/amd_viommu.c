#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/vfio/pci.h"
#include "hw/core/iommu.h"
#include "system/runstate.h"
#include "hw/vfio/vfio-device.h"
#include "hw/vfio/vfio-iommufd.h"
#include "system/kvm.h"

#include "amd_iommu_common.h"
#include "amd_viommu.h"
#include "trace.h"

DEFINE_AMD_IOMMU_SET_QUAD(amd_viommu, AMDVIOMMUState);

DEFINE_AMD_VIOMMU_READ(w, uint16_t, lduw_le_p)
DEFINE_AMD_VIOMMU_READ(l, uint32_t, ldl_le_p)
DEFINE_AMD_VIOMMU_READ(q, uint64_t, ldq_le_p)

DEFINE_AMD_IOMMU_MMIO_REG_READ(amd_viommu, AMDVIOMMUState)

DEFINE_AMD_VIOMMU_WRITE(w, uint16_t, lduw_le_p, stw_le_p)
DEFINE_AMD_VIOMMU_WRITE(l, uint32_t, ldl_le_p, stl_le_p)
DEFINE_AMD_VIOMMU_WRITE(q, uint64_t, ldq_le_p, stq_le_p)

DEFINE_AMD_IOMMU_MMIO_REG_WRITE(amd_viommu, AMDVIOMMUState)

static uint64_t amd_viommu_dte_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t val = 0;
    AMDIOMMUFDDevice *amd_idev = opaque;

    if (size ==  2) {
        val = lduw_le_p(&amd_idev->dte_info.dte8[offset]);
    } else if (size == 4) {
        val = ldl_le_p(&amd_idev->dte_info.dte8[offset]);
    } else if (size == 8) {
        val = ldq_le_p(&amd_idev->dte_info.dte8[offset]);
    }

    return val;
}

static int amd_viommu_device_attach_nested_hwpt(AMDIOMMUFDDevice *amd_idev,
                                                uint32_t nested_hwpt_id)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    uint32_t old_nested_hwpt_id = amd_idev->nested_hwpt_id;
    uint32_t pt_nested_hwpt_id = amd_idev->viommu->iommufd_state.pt_nested_hwpt_id;
    Error *local_err = NULL;
    int ret = 0;

    if (old_nested_hwpt_id == nested_hwpt_id) {
        return ret;
    }

    if (!host_iommu_device_iommufd_attach_hwpt(idev, IOMMU_NO_PASID,
                                               nested_hwpt_id, &local_err)) {
        error_reportf_err(local_err, "Failed to attach nested HWPT: ");
        ret = -EINVAL;
    }

    amd_idev->nested_hwpt_id = nested_hwpt_id;

    trace_amd_viommu_attach_nested_hwpt(idev->devid, old_nested_hwpt_id,
                                        nested_hwpt_id, ret);

    if (!ret && old_nested_hwpt_id &&
        old_nested_hwpt_id != pt_nested_hwpt_id) {
        iommufd_backend_free_id(amd_idev->viommu->iommufd_state.iommufd,
                                old_nested_hwpt_id);
        return ret;
    }

    if (ret) {
        if (old_nested_hwpt_id) {
            host_iommu_device_iommufd_attach_hwpt(idev, IOMMU_NO_PASID,
                                                  old_nested_hwpt_id, NULL);
            amd_idev->nested_hwpt_id = old_nested_hwpt_id;
        } else {
            /* Attach to passthrough hwpt*/
            host_iommu_device_iommufd_attach_hwpt(idev, IOMMU_NO_PASID,
                                                  pt_nested_hwpt_id, NULL);
            amd_idev->nested_hwpt_id = pt_nested_hwpt_id;
        }
    }

    return ret;
}

static int amd_viommu_update_gcr3(AMDIOMMUFDDevice *amd_idev)
{
    int ret;
    uint32_t nested_hwpt_id;
    Error *local_err = NULL;
    struct iommu_hwpt_amd_guest hwpt;
    uint32_t dev_id = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod)->devid;
    AMDVIOMMUState *viommu = amd_idev->viommu;

    memcpy(&hwpt.dte, &amd_idev->dte_info.dte8, AMDVI_MMIO_DEVTAB_ENTRY_SIZE);

    /* Calling drivers/iommu/amd/viommu.c: _amd_viommu_alloc_domain_nested() */
    if (!iommufd_backend_alloc_hwpt(viommu->iommufd_state.iommufd, dev_id,
                                    viommu->iommufd_state.viommu_id, 0,
                                    IOMMU_HWPT_DATA_AMD_GUEST, sizeof(hwpt),
                                    &hwpt, &nested_hwpt_id, &local_err)) {
        error_report_err(local_err);
        return -EINVAL;
    }

    ret = amd_viommu_device_attach_nested_hwpt(amd_idev, nested_hwpt_id);

    if (ret) {
        error_report("Could not attach device %d to nested HWPT %d", dev_id, nested_hwpt_id);
        iommufd_backend_free_id(viommu->iommufd_state.iommufd, nested_hwpt_id);
    }

    trace_amd_viommu_update_gcr3(dev_id, nested_hwpt_id);

    return ret;
}

static uint64_t get_gcr3_trp(uint64_t *dte)
{
    uint64_t gcr3_14_12, gcr3_30_15, gcr3_51_31;

    gcr3_14_12 = (dte[0] & AMDVI_DEV_GCR3_14_12_MASK) >>
                 AMDVI_DEV_GCR3_14_12_SHIFT;
    gcr3_30_15 = (dte[1] & AMDVI_DEV_GCR3_30_15_MASK) >>
                 AMDVI_DEV_GCR3_30_15_SHIFT;
    gcr3_51_31 = (dte[1] & AMDVI_DEV_GCR3_51_31_MASK) >>
                 AMDVI_DEV_GCR3_51_31_SHIFT;

    return (gcr3_51_31 << 31) | (gcr3_30_15 << 15) | (gcr3_14_12 << 12);
}

static bool amd_viommu_validate_dte(uint64_t *dte)
{
    bool valid, transl_valid, guest_transl_valid;
    uint8_t gpt_level, glx;
    uint64_t gcr3_trp;

    if ((dte[0] & AMDVI_DEV_QUAD0_RESERVED_MASK) ||
        (dte[1] & AMDVI_DEV_QUAD1_RESERVED_MASK) ||
        (dte[2] & AMDVI_DEV_QUAD2_RESERVED_MASK) ||
        (dte[3] & AMDVI_DEV_QUAD3_RESERVED_MASK)) {
        error_report("DTE reserve region set");
        trace_amd_viommu_invalid_dte("reserved bits set");
        return false;
    }

    valid              = dte[0] & AMDVI_DEV_VALID;
    transl_valid       = dte[0] & AMDVI_DEV_TRANSLATION_VALID;
    guest_transl_valid = dte[0] & AMDVI_DEV_G_TRANSLATION_VALID;

    if (!(valid && transl_valid && guest_transl_valid)) {
        /* Skip the translation */
        trace_amd_viommu_invalid_dte("translation not enabled");
        return false;
    }

    gcr3_trp = get_gcr3_trp(dte);

    if (!gcr3_trp) {
        error_report("Invalid GCR3 address");
        trace_amd_viommu_invalid_dte("invalid gcr3 address");
        return false;
    }

    gpt_level = (dte[2] & AMDVI_DEV_G_PAGING_MODE_MASK) >>
                AMDVI_DEV_G_PAGING_MODE_SHIFT;

    if (gpt_level != AMDVI_G_PGTABLE_4_LEVEL &&
        gpt_level != AMDVI_G_PGTABLE_5_LEVEL) {
        error_report("Invalid guest page table level");
        trace_amd_viommu_invalid_dte("invalid guest page table level");
        return false;
    }

    glx = (dte[0] & AMDVI_DEV_GLX_MASK) >> AMDVI_DEV_GLX_SHIFT;
    if (glx == 3) {
        error_report("Reserved GLX value (3)");
        trace_amd_viommu_invalid_dte("reserved glx value");
        return false;
    }

    return true;
}

static void amd_viommu_dte_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    AMDIOMMUFDDevice *amd_idev = opaque;
    int ret = 0;

    /* Storing value in the devtab */
    if (size ==  2) {
        stw_le_p(&amd_idev->dte_info.dte8[offset], val);
    } else if (size == 4) {
        stl_le_p(&amd_idev->dte_info.dte8[offset], val);
    } else if (size == 8) {
        stq_le_p(&amd_idev->dte_info.dte8[offset], val);
    }

    /* Ignore writes to the second half of the DTE */
    if (offset > AMDVI_MMIO_DEVTAB_ENTRY_SIZE / 2) {
        return;
    }

    trace_amd_viommu_dte_write(offset, size,
                               amd_idev->dte_info.dte64[0],
                               amd_idev->dte_info.dte64[1],
                               amd_idev->dte_info.dte64[2],
                               amd_idev->dte_info.dte64[3]);

    /* Write when guest has completed write to first 128 bits */
    if (((amd_idev->dte_info.last_offset == 0 && offset == 8) ||
         (amd_idev->dte_info.last_offset == 8 && offset == 0)) &&
        amd_viommu_validate_dte(amd_idev->dte_info.dte64)) {
        ret = amd_viommu_update_gcr3(amd_idev);
        if (!ret) {
            amd_idev->dte_info.last_offset = -1;
        }
    } else if (offset == 0 || offset == 8) {
        amd_idev->dte_info.last_offset = offset;
    }

    return;
}

static void __amd_viommu_reset_dev_hwpt(AMDIOMMUFDDevice *amd_idev)
{
    uint32_t dev_id = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod)->devid;
    int ret;

    if (!amd_idev->vdevice_id) {
        return;
    }

    memset(&amd_idev->dte_info.dte8, 0, AMDVI_MMIO_DEVTAB_ENTRY_SIZE);

    trace_amd_viommu_reset_dev_hwpt(
            dev_id, amd_idev->viommu->iommufd_state.pt_nested_hwpt_id);

    ret = amd_viommu_device_attach_nested_hwpt(
                            amd_idev,
                            amd_idev->viommu->iommufd_state.pt_nested_hwpt_id);

    if (ret) {
        warn_report("could not attach vdevice id %d to passthrough domain",
                    dev_id);
    }
}

static void amd_viommu_reset_dev_hwpt(gpointer key, gpointer value,
				                      gpointer user_data)
{
    AMDIOMMUFDDevice *amd_idev = value;

    __amd_viommu_reset_dev_hwpt(amd_idev);
}

static void amd_viommu_reset_device_hwpt_all(AMDVIOMMUState *s)
{
    g_hash_table_foreach(s->iommufd_state.iommufd_dev_hash,
                         amd_viommu_reset_dev_hwpt,
                         NULL);
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

static void amd_viommu_dealloc_dte_as(gpointer key, gpointer value,
				                   gpointer user_data)
{
    struct AMDDeviceKey *bus_devfn = key;
    AMDIOMMUFDDevice *amd_idev = value;

    trace_amd_viommu_dealloc_dte_as(pci_bus_num(bus_devfn->bus),
                                    PCI_SLOT(bus_devfn->devfn),
                                    PCI_FUNC(bus_devfn->devfn));

    memory_region_del_subregion(get_system_memory(), &amd_idev->mr_dte);
    object_unparent(OBJECT(&amd_idev->mr_dte));
}

static void amd_viommu_dealloc_devtab(AMDVIOMMUState *s)
{
    if (!s->devtab_size)
        return;

    g_hash_table_foreach(s->iommufd_state.iommufd_dev_hash,
                         amd_viommu_dealloc_dte_as,
                         NULL);
    s->devtab_base = 0;
    s->devtab_size = 0;
}

static void amd_viommu_alloc_dte_as(gpointer key, gpointer value,
				                  gpointer user_data)
{
    struct AMDDeviceKey *bus_devfn = key;
    AMDIOMMUFDDevice *amd_idev = value;
    AMDVIOMMUState *viommu = amd_idev->viommu;
    hwaddr devtab_base = viommu->devtab_base;
    uint16_t bdf;
    uint64_t offset;
    uint64_t devtab_end = devtab_base + viommu->devtab_size;
    char name[30];

    bdf = PCI_BUILD_BDF(pci_bus_num(bus_devfn->bus), bus_devfn->devfn);
    offset = devtab_base + bdf * AMDVI_MMIO_DEVTAB_ENTRY_SIZE;

    if (offset + AMDVI_MMIO_DEVTAB_ENTRY_SIZE > devtab_end) {
        error_report("inadequate device table size, "
                     "cannot allocate AS for DeviceID: %02x:%02x.%01x",
                     PCI_BUS_NUM(bdf), PCI_SLOT(bdf), PCI_FUNC(bdf));
        return;
    }

    snprintf(name, 30, "%s-%02u", "amd-iommu-devtab", amd_idev->viommu->iommu.index);
    memory_region_init_io(&amd_idev->mr_dte, OBJECT(amd_idev->viommu),
            &dte_ops, amd_idev, name,
            AMDVI_MMIO_DEVTAB_ENTRY_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), offset,
            &amd_idev->mr_dte, 1);

    trace_amd_viommu_alloc_dte_as(PCI_BUS_NUM(bdf), PCI_SLOT(bdf),
                                  PCI_FUNC(bdf), offset);
}

static inline void amd_viommu_handle_devtab_write(AMDVIOMMUState *s)
{
    uint64_t val = amd_viommu_readq(s, AMDVI_MMIO_DEVICE_TABLE);

    if (s->devtab_size) {
        amd_viommu_dealloc_devtab(s);
    }

    s->devtab_base = (val & AMDVI_MMIO_DEVTAB_BASE_MASK);

    /*
     * The devtab_size is the size specifyied by guest indicated is
     * (n + 1) * 4 Kbytes
     */
    s->devtab_size = ((val & AMDVI_MMIO_DEVTAB_SIZE_MASK) + 1) <<
                     AMDVI_PAGE_SHIFT_4K;

    trace_amd_viommu_devtab_write(s->devtab_base, s->devtab_size);

    /*
     * Set up memory listener for IOMMU DTE of the devices registered via
     * set_iommu_device.
     */
    g_hash_table_foreach(s->iommufd_state.iommufd_dev_hash,
                         amd_viommu_alloc_dte_as,
                         NULL);
}

static void amd_viommu_intcapxt_decode(uint64_t val, uint32_t *vcpu_id,
                                       uint8_t *vector)
{
    uint32_t dest_lo = (val >> 8) & 0xffffff;
    uint32_t dest_hi = (val >> 56) & 0xff;

    *vcpu_id = (dest_hi << 24) | dest_lo;
    *vector = (val >> 32) & 0xff;
}

static int amd_viommu_ext_int_remap(AMDVIOMMUState *s, uint32_t type,
                                    uint64_t val)
{
    uint32_t vcpu_id;
    uint8_t vector;
    int kvmfd;
    int ret = 0;
    Error *local_err = NULL;

    if (!val) {
        return 0;
    }

    if (!s->iommufd_state.viommu_id) {
        error_report("%s: Unitialized vIOMMU", __func__);
        exit(EXIT_FAILURE);
    }

    amd_viommu_intcapxt_decode(val, &vcpu_id, &vector);

    kvmfd = kvm_vmfd(kvm_state);
    if (kvmfd < 0) {
        error_report("XT registers requires valid KVM VM fd");
        return -EINVAL;
    }

    if (!iommufd_viommu_ext_int_remap(s->iommufd_state.iommufd,
                                      s->iommufd_state.viommu_id, type,
                                      kvmfd, vcpu_id, vector, &local_err)) {
        error_reportf_err(local_err, "Failed to remap extended interrupt: ");
        ret = -EINVAL;
    }

    trace_amd_viommu_ext_int_remap(type, vcpu_id, vector, ret);

    return ret;
}

static inline void amd_viommu_handle_xt_event_int_write(AMDVIOMMUState *s)
{
    uint64_t val = amd_viommu_readq(s, AMDVI_MMIO_XT_GEN_INTR);

    trace_amd_viommu_xt_int_write("AMDVI_MMIO_XT_GEN_INTR", val);
    amd_viommu_ext_int_remap(s, IOMMU_VIOMMU_EXT_INT_EVENT, val);
}

static inline void amd_viommu_handle_xt_ppr_int_write(AMDVIOMMUState *s)
{
    uint64_t val = amd_viommu_readq(s, AMDVI_MMIO_XT_PPR_INT);

    trace_amd_viommu_xt_int_write("AMDVI_MMIO_XT_PPR_INT", val);
    amd_viommu_ext_int_remap(s, IOMMU_VIOMMU_EXT_INT_PPR, val);
}

static uint32_t amd_viommu_get_cmdbuf_flag(AMDVIOMMUState *s)
{
    uint64_t ctrl = amd_viommu_readq(s, AMDVI_MMIO_CONTROL);
    uint32_t flags = 0;

    if (ctrl & AMDVI_MMIO_CONTROL_CMDBUFLEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_CMDBUF_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_COMWAITINTEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_COMWAIT_EN;

    return flags;
}

static uint32_t amd_viommu_get_evtbuf_flag(AMDVIOMMUState *s)
{
    uint64_t ctrl = amd_viommu_readq(s, AMDVI_MMIO_CONTROL);
    uint32_t flags = 0;

    if (ctrl & AMDVI_MMIO_CONTROL_EVENTLOGEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_EVT_LOG_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_EVENTINTEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_EVT_INT_EN;

    return flags;
}

static uint32_t amd_viommu_get_pprbuf_flag(AMDVIOMMUState *s)
{
    uint64_t ctrl = amd_viommu_readq(s, AMDVI_MMIO_CONTROL);
    uint32_t flags = 0;

    if (ctrl & AMDVI_MMIO_CONTROL_PPRLOGEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPRLOG_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_PPRINTEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPRINT_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_PPREN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPR_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_PPRAUTORSPEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPR_AUTO_RSP_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_BLKSTOPMRKEN)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_BLKSTOPMRK_EN;
    if (ctrl & AMDVI_MMIO_CONTROL_PPRAUTORSPAON)
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPR_AUTO_RSP_AON;

    return flags;
}

static void amd_viommu_dealloc_queue(AMDVIOMMUState *s,
                                     AMDVIOMMUHwQueue *queue)
{
    if (!queue->enable_hw_queue) {
        return;
    }

    trace_amd_viommu_dealloc_hw_queue(queue->type, queue->hw_queue_id);

    iommufd_backend_free_id(s->iommufd_state.iommufd, queue->hw_queue_id);
    queue->hw_queue_id = 0;
    queue->enable_hw_queue = false;
}

static void amd_viommu_alloc_queue(AMDVIOMMUState *s, AMDVIOMMUHwQueue *queue)
{
    uint32_t flags = 0;
    uint64_t mmio_val = 0;
    uint64_t addr, len;
    const char *queue_name = "";
    Error *local_err = NULL;

    switch (queue->type) {
    case IOMMU_HW_QUEUE_TYPE_AMD_CMD:
        flags = amd_viommu_get_cmdbuf_flag(s);
        mmio_val = amd_viommu_readq(s, AMDVI_MMIO_COMMAND_BASE);
        queue_name = "COMMAND";
        break;
    case IOMMU_HW_QUEUE_TYPE_AMD_EVT:
        flags = amd_viommu_get_evtbuf_flag(s);
        mmio_val = amd_viommu_readq(s, AMDVI_MMIO_EVENT_BASE);
        queue_name = "EVENT";
        break;
    case IOMMU_HW_QUEUE_TYPE_AMD_PPR:
        flags = amd_viommu_get_pprbuf_flag(s);
        mmio_val = amd_viommu_readq(s, AMDVI_MMIO_PPR_BASE);
        queue_name = "PPR";
        break;
    }

    addr = mmio_val & AMDVI_MMIO_BUF_BASE_MASK;
    len = (mmio_val & AMDVI_MMIO_BUF_SIZE_MASK) >> AMDVI_MMIO_BUF_SIZE_SHIFT;

    if (queue->enable_hw_queue && queue->addr == addr && queue->len == len &&
        queue->flags == flags) {
        return;
    }

    if (queue->enable_hw_queue) {
        amd_viommu_dealloc_queue(s, queue);
    }

    if (!iommufd_backend_alloc_hw_queue(s->iommufd_state.iommufd,
                                        s->iommufd_state.viommu_id,
                                        queue->type, 0, addr, len, flags,
                                        &queue->hw_queue_id, &local_err)) {
        error_reportf_err(local_err, "Failed to allocate %s buffer: ",
                          queue_name);
        return;
    }

    queue->addr = addr;
    queue->len = len;
    queue->flags = flags;
    queue->enable_hw_queue = true;

    trace_amd_viommu_alloc_hw_queue(queue_name, addr, len, flags,
                                    queue->hw_queue_id);
}

static void amd_viommu_alloc_vdev(gpointer key, gpointer value, gpointer user_data)
{
    AMDDeviceKey *bus_devfn = key;
    AMDIOMMUFDDevice *amd_idev = value;
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    uint16_t bdf = PCI_BUILD_BDF(pci_bus_num(bus_devfn->bus), bus_devfn->devfn);
    AMDIOMMUFDState *iommufd_state = &amd_idev->viommu->iommufd_state;
    Error *local_err = NULL;

    if (!iommufd_backend_alloc_vdev(idev->iommufd, idev->devid,
                                    iommufd_state->viommu_id, bdf,
                                    &amd_idev->vdevice_id, &local_err)) {
        error_reportf_err(local_err,
                          "Could not allocate vdevice for PCI DeviceID "
                          "%02x:%02x.%01x: ",
                          PCI_BUS_NUM(bdf), PCI_SLOT(bdf), PCI_FUNC(bdf));
        exit(EXIT_FAILURE);
    }

    trace_amd_viommu_alloc_vdev(PCI_BUS_NUM(bdf), PCI_SLOT(bdf),
                                PCI_FUNC(bdf), amd_idev->vdevice_id);
}

static void amd_viommu_alloc_vdev_all(AMDIOMMUFDState *s)
{
    g_hash_table_foreach(s->iommufd_dev_hash, amd_viommu_alloc_vdev, NULL);
}

static void amd_viommu_dealloc_vdev(gpointer key, gpointer value, gpointer user_data)
{
    AMDIOMMUFDDevice *amd_idev = value;

    if (!amd_idev->vdevice_id) {
        return;
    }

    /*
     * Attach vdevice to passthrough domain before deallocating it so that
     * DMAs initiated after vdevice dealloc works fine.
     */
    __amd_viommu_reset_dev_hwpt(amd_idev);

    trace_amd_viommu_dealloc_vdev(amd_idev->vdevice_id);

    iommufd_backend_free_id(amd_idev->viommu->iommufd_state.iommufd,
                            amd_idev->vdevice_id);
    amd_idev->vdevice_id = 0;
}

static void amd_viommu_dealloc_vdev_all(AMDIOMMUFDState *s)
{
    g_hash_table_foreach(s->iommufd_dev_hash, amd_viommu_dealloc_vdev, NULL);
}


static inline void amd_viommu_handle_control_write(AMDVIOMMUState *s)
{
    unsigned long ctrl = amd_viommu_readq(s, AMDVI_MMIO_CONTROL);
    bool enable_iommu = !!(ctrl & AMDVI_MMIO_CONTROL_AMDVIEN);
    bool enable_hw_queue;

#define CONTROL_BIT_ENABLED(state, name) (state.name == 0 && name == 1)
#define CONTROL_BIT_DISABLED(state, name) (state.name == 1 && name == 0)

    trace_amd_viommu_control_write(ctrl);

    if (CONTROL_BIT_ENABLED(s->iommufd_state, enable_iommu)) {
        amd_viommu_alloc_vdev_all(&s->iommufd_state);
        s->iommufd_state.enable_iommu = true;
    } else if (CONTROL_BIT_DISABLED(s->iommufd_state, enable_iommu)) {
        amd_viommu_dealloc_vdev_all(&s->iommufd_state);
        s->iommufd_state.enable_iommu = false;
    }

    enable_hw_queue = !!(ctrl & AMDVI_MMIO_CONTROL_CMDBUFLEN);
    if (CONTROL_BIT_ENABLED(s->cmd_queue, enable_hw_queue)) {
        amd_viommu_alloc_queue(s, &s->cmd_queue);
    } else if (CONTROL_BIT_DISABLED(s->cmd_queue, enable_hw_queue)) {
        amd_viommu_dealloc_queue(s, &s->cmd_queue);
    }

    enable_hw_queue = !!(ctrl & AMDVI_MMIO_CONTROL_EVENTLOGEN);
    if (CONTROL_BIT_ENABLED(s->evt_queue, enable_hw_queue)) {
        amd_viommu_alloc_queue(s, &s->evt_queue);
    } else if (CONTROL_BIT_DISABLED(s->evt_queue, enable_hw_queue)) {
        amd_viommu_dealloc_queue(s, &s->evt_queue);
    }

    enable_hw_queue = !!(ctrl & AMDVI_MMIO_CONTROL_PPRLOGEN);
    if (CONTROL_BIT_ENABLED(s->ppr_queue, enable_hw_queue)) {
        amd_viommu_alloc_queue(s, &s->ppr_queue);
    } else if (CONTROL_BIT_DISABLED(s->ppr_queue, enable_hw_queue)) {
        amd_viommu_dealloc_queue(s, &s->ppr_queue);
    }

#undef CONTROL_BIT_ENABLED
#undef CONTROL_BIT_DISABLED
}

static uint64_t amd_viommu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AMDVIOMMUState *s = opaque;

    uint64_t val = -1;
    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amd_viommu_mmio_read_invalid(AMDVI_MMIO_SIZE, addr, size);
        return (uint64_t)-1;
    }

    val = amd_viommu_mmio_reg_read(s, size, addr);
    trace_amd_viommu_mmio_read(amdvi_mmio_get_name(addr), addr, size,
                               addr & ~0x07);
    return val;
}

static void amd_viommu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    AMDVIOMMUState *s = opaque;
    unsigned long offset = addr & 0x07;

    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amd_viommu_mmio_write_invalid(AMDVI_MMIO_SIZE, addr, size, val);
        return;
    }

    trace_amd_viommu_mmio_write(amdvi_mmio_get_name(addr), addr, size, val,
                                offset);

    amd_viommu_mmio_reg_write(s, size, val, addr);
    switch (addr & ~0x07) {
    case AMDVI_MMIO_CONTROL:
        /* FIXME - make sure System Software has finished writing incase
         * it writes in chucks less than 8 bytes in a robust way.As for
         * now, this hacks works for the linux driver
         */
        if (offset || (size == 8)) {
            amd_viommu_handle_control_write(s);
        }
        break;
    case AMDVI_MMIO_XT_GEN_INTR:
        if (!offset && size == 8) {
            amd_viommu_handle_xt_event_int_write(s);
        }
        break;
    case AMDVI_MMIO_XT_PPR_INT:
        if (!offset && size == 8) {
            amd_viommu_handle_xt_ppr_int_write(s);
        }
        break;
    case AMDVI_MMIO_DEVICE_TABLE:
       /*
        *   set device table address
        *   This also suffers from inability to tell whether software
        *   is done writing
        */
        if (offset || (size == 8)) {
            amd_viommu_handle_devtab_write(s);
        }
        break;
    }
}

static void amd_viommu_init_mmio(AMDVIOMMUState *s)
{
    memset(s->mmior, 0, AMDVI_MMIO_SIZE);
    memset(s->w1cmask, 0, AMDVI_MMIO_SIZE);
    memset(s->romask, 0, AMDVI_MMIO_SIZE);

    amd_viommu_set_quad(s, AMDVI_MMIO_EXT_FEATURES,
                        0,
                        0xffffffffffffffff, 0);
    amd_viommu_set_quad(s, AMDVI_MMIO_STATUS, 0, 0x98, 0x67);
}

static const MemoryRegionOps mmio_mem_ops = {
    .read = amd_viommu_mmio_read,
    .write = amd_viommu_mmio_write,
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

static bool amd_viommu_init_accel_mmio(AMDVIOMMUState *s)
{
    AMDIOMMUFDState *iommufd_state = &s->iommufd_state;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint64_t vf_base_addr = AMDVI_GET_BASE_ADDR(x86_iommu->index) +
                            AMD_VIOMMU_VF_MMIO_BASE;
    off_t mmap_offset =
        iommufd_state->iommufd_viommu_amd.out_vfmmio_mmap_offset;
    Error *local_err = NULL;
    char *name;

    if (!iommufd_backend_viommu_mmap(iommufd_state->iommufd,
                                     iommufd_state->viommu_id,
                                     AMDVI_PAGE_SIZE, mmap_offset,
                                     &s->vf_mmio_page, &local_err)) {
        error_reportf_err(local_err, "Failed to mmap VF MMIO: ");
        s->vf_mmio_page = NULL;
        return false;
    }

    name = g_strdup_printf("vf-mmio");
    memory_region_init_ram_device_ptr(&s->mr_vf_mmio, OBJECT(s), name,
                                      AMD_VIOMMU_VF_MMIO_SIZE,
                                      s->vf_mmio_page);
    /*
     * This is a vf-mmio region that is not used for DMA.
     * It is needs to be mapped as RAM in order to allow CPU nested pagetable
     * to map for the vf-mmio region.
     */
    memory_region_set_skip_iommu_map(&s->mr_vf_mmio, true);

    memory_region_add_subregion_overlap(get_system_memory(),
                                        vf_base_addr,
                                        &s->mr_vf_mmio,
                                        1);

    trace_amd_viommu_vf_mmio_map(vf_base_addr, AMD_VIOMMU_VF_MMIO_SIZE,
                                 mmap_offset);

    g_free(name);
    return true;
}

static bool amd_viommu_init_iommufd(AMDIOMMUFDState *s)
{
    struct iommu_hwpt_amd_guest pt_hwpt = {{0, 0, 0, 0}};
    Error *local_err = NULL;

    if (!iommufd_backend_alloc_viommu(s->iommufd, s->devid,
                                      IOMMU_VIOMMU_TYPE_AMD,
                                      s->parent_hwpt_id,
                                      &s->iommufd_viommu_amd,
                                      sizeof(s->iommufd_viommu_amd),
                                      &s->viommu_id, &local_err)) {
        error_reportf_err(local_err,
                          "Failed to allocate a amd-viommu iommufd type: ");
        return false;
    }

    if (!s->pt_nested_hwpt_id) {
        if (!iommufd_backend_alloc_hwpt(s->iommufd, s->devid,
                                        s->viommu_id, 0,
                                        IOMMU_HWPT_DATA_AMD_GUEST,
                                        sizeof(pt_hwpt), &pt_hwpt,
                                        &s->pt_nested_hwpt_id, &local_err)) {
            error_reportf_err(local_err,
                              "Failed to allocate passthrough domain: ");
            return false;
        }
    }

    trace_amd_viommu_init_iommufd(s->viommu_id, s->devid, s->parent_hwpt_id);

    return true;
}

static void amd_viommu_state_change_running(AMDVIOMMUState *s)
{
    if (s->iommufd_state.viommu_id)
        return;

    if (!amd_viommu_init_iommufd(&s->iommufd_state)) {
        exit(EXIT_FAILURE);
    }

    if (!amd_viommu_init_accel_mmio(s)) {
        exit(EXIT_FAILURE);
    }

    return;
}

static void amd_viommu_vm_state_change(void *opaque,
                                        bool running, RunState state)
{
    AMDVIOMMUState *s = opaque;

    trace_amd_viommu_vm_state_change(RunState_str(state));

    switch (state) {
    case RUN_STATE_RUNNING:
        amd_viommu_state_change_running(s);
        break;
    default:
        break;
    }
}

static uint64_t amd_viommu_extended_feature_register(X86IOMMUState *x86_iommu)
{
    AMDVIOMMUState *s = AMD_VIOMMU_DEVICE(x86_iommu);
    return s->hwinfo->efr & AMD_VIOMMU_SUPPORTED_EFR;
}

static void amd_viommu_set_efrs_hwinfo(AMDVIOMMUState *s,
                                       struct iommu_hw_info_amd *hwinfo)
{
    uint64_t efr;

    s->hwinfo = hwinfo;
    efr = amd_viommu_extended_feature_register(&s->iommu);
    amd_viommu_set_quad(s, AMDVI_MMIO_EXT_FEATURES, efr,
                        0xffffffffffffffff, 0);

    trace_amd_viommu_efr(hwinfo->efr, efr);
}

static void amd_viommu_unset_iommu_device(PCIBus *bus, void *opaque,
                                          int devfn)
{
    AMDVIOMMUState *s = opaque;
    AMDIOMMUFDDevice *amd_idev;
    AMDDeviceKey key = {
        .bus = bus,
        .devfn = devfn,
    };

    if (!(amd_idev = g_hash_table_lookup(s->iommufd_state.iommufd_dev_hash, &key))) {
        return;
    }

    trace_amd_viommu_unset_iommu_device(pci_bus_num(bus), PCI_SLOT(devfn),
                                        PCI_FUNC(devfn));

    memset(&amd_idev->dte_info.dte8, 0, AMDVI_MMIO_DEVTAB_ENTRY_SIZE);
    host_iommu_device_iommufd_attach_hwpt(
                                HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod),
                                IOMMU_NO_PASID,
                                s->iommufd_state.pt_nested_hwpt_id, NULL);

    g_hash_table_remove(s->iommufd_state.iommufd_dev_hash, &key);
}

/*
 * Return the canonical sysfs path of the physical IOMMU that owns @vdev,
 * e.g. "/sys/class/iommu/ivhd1". Caller must g_free() the result.
 *
 * Requires @vdev->sysfsdev and kernel iommu probe (device must have
 * .../iommu symlink — see Documentation/ABI/testing/sysfs-class-iommu).
 */
static char *amd_viommu_sysfs_phys_iommu_path(VFIODevice *vdev, Error **errp)
{
    g_autofree char *link = NULL;
    char *canonical;
    if (!vdev->sysfsdev) {
        error_setg(errp, "no host sysfs path");
        return NULL;
    }
    link = g_strdup_printf("%s/iommu", vdev->sysfsdev);
    canonical = realpath(link, NULL);
    if (!canonical) {
        error_setg_errno(errp, errno, "no physical IOMMU sysfs link at %s",
                         link);
        return NULL;
    }
    return canonical;
}

static const char *amd_viommu_phys_iommu_name(const char *path)
{
    return path ? g_path_get_basename(path) : "unknown";
}

static bool verify_device_physical_iommu(AMDVIOMMUState *s, VFIODevice *vdev,
                                         Error **errp)
{
    g_autofree char *phys_iommu_path;

    phys_iommu_path = amd_viommu_sysfs_phys_iommu_path(vdev, errp);
    if (!phys_iommu_path) {
        /*
         * Defer the verification to kernel, the vdevice alloc will fail if
         * the device does not belong to given iommu.
         */
        warn_report("amd-viommu: cannot verify physical IOMMU for device %s: %s",
                    vdev->name, error_get_pretty(*errp));
        error_free(*errp);
        *errp = NULL;
    } else if (!s->phys_iommu_path) {
        s->phys_iommu_path = g_steal_pointer(&phys_iommu_path);
    } else if (strcmp(s->phys_iommu_path, phys_iommu_path)) {
        error_setg(errp,
                   "device %s is on host IOMMU %s, but amd-viommu "
                   "already has devices on %s",
                   vdev->name,
                   amd_viommu_phys_iommu_name(phys_iommu_path),
                   amd_viommu_phys_iommu_name(s->phys_iommu_path));
        return false;
    }

    return true;
}

static bool amd_viommu_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                   HostIOMMUDevice *hiod, Error **errp)
{
    AMDVIOMMUState *s = opaque;
    AMDIOMMUFDDevice *amd_idev;
    AMDIOMMUFDState *iommufd_state = &s->iommufd_state;
    AMDDeviceKey *new_key;
    AMDDeviceKey key = { .bus = bus, .devfn = devfn };
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    VFIODevice *vdev = hiod->agent;

    assert(hiod);
    assert(vdev);

    trace_amd_viommu_set_iommu_device(pci_bus_num(bus), PCI_SLOT(devfn),
                                      PCI_FUNC(devfn));

    if (hiod->caps.type != IOMMU_HW_INFO_TYPE_AMD) {
        error_setg(errp, "IOMMU hardware type %#x is not compatible", hiod->caps.type);
        return false;
    }

    if (g_hash_table_lookup(iommufd_state->iommufd_dev_hash, &key)) {
        error_setg(errp, "Host IOMMU device already exist");
        return false;
    }

    if (!iommufd_state->iommufd) {
        iommufd_state->iommufd = idev->iommufd;
    } else if (iommufd_state->iommufd != idev->iommufd) {
        error_setg(errp, "Devices attached to multiple iommufds are not allowed"
                         "Under single amd-viommu");
        return false;
    }

    if (!vdev->hwpt || !(vdev->hwpt->hwpt_flags & IOMMU_HWPT_ALLOC_NEST_PARENT)) {
        error_setg(errp, "device %s missing VFIO nesting parent HWPT "
                "(VIOMMU_FLAG_WANT_NESTING_PARENT)",
                vdev->name);
        return false;
    }

    if (!verify_device_physical_iommu(s, vdev, errp)) {
        return false;
    }

    if (!s->hwinfo) {
        amd_viommu_set_efrs_hwinfo(s, &hiod->caps.vendor_caps.amd);
    }

    if (!iommufd_state->parent_hwpt_id) {
        iommufd_state->parent_hwpt_id = vdev->hwpt->hwpt_id;
    } else if (iommufd_state->parent_hwpt_id != vdev->hwpt->hwpt_id) {
        error_setg(errp, "device %s HWPT ID does not match previous HWPT ID",
                   vdev->name);
        return false;
    }

    if (!s->iommufd_state.devid) {
        s->iommufd_state.devid = idev->devid;
    }

    new_key = g_malloc(sizeof(AMDDeviceKey));
    new_key->bus = bus;
    new_key->devfn = devfn;

    amd_idev = g_malloc0(sizeof(AMDIOMMUFDDevice));
    amd_idev->viommu = s;
    amd_idev->hiod = hiod;
    amd_idev->nested_hwpt_id = 0;
    amd_idev->dte_info.last_offset = -1;
    memset(amd_idev->dte_info.dte8, 0, AMDVI_MMIO_DEVTAB_ENTRY_SIZE);

    g_hash_table_insert(iommufd_state->iommufd_dev_hash, new_key, amd_idev);

    return true;
}

static AddressSpace *amd_viommu_get_address_space(PCIBus *bus, void *opaque,
                                                  int devfn)
{
    return &address_space_memory;
}

static int amd_viommu_get_x86_iommu(void *opaque, void **x86_iommu)
{
    AMDVIOMMUState *s = opaque;
    *x86_iommu = X86_IOMMU_DEVICE(s);
    return 0;
}

static uint64_t amd_viommu_get_viommu_flags(void *opaque)
{
    return VIOMMU_FLAG_WANT_NESTING_PARENT;
}

static const PCIIOMMUOps amd_viommu_pci_ops = {
    .get_address_space = amd_viommu_get_address_space,
    .set_iommu_device = amd_viommu_set_iommu_device,
    .unset_iommu_device = amd_viommu_unset_iommu_device,
    .get_x86_iommu = amd_viommu_get_x86_iommu,
    .get_viommu_flags = amd_viommu_get_viommu_flags,
};

static void amd_viommu_init_queue(AMDVIOMMUHwQueue *queue, uint32_t type)
{
    queue->hw_queue_id = 0;
    queue->enable_hw_queue = false;
    queue->type = type;
    queue->flags = 0;
}

/* Use stub for now, future patches will add states to init here */
static void amd_viommu_init(AMDVIOMMUState *s)
{
    s->iommufd_state.iommufd_dev_hash = g_hash_table_new_full(
                                                amd_iommu_device_key_fn,
                                                amd_iommu_device_key_equal,
                                                g_free, g_free);
    s->hwinfo = NULL;
    s->ivrs_info.get_extended_feature_register = amd_viommu_extended_feature_register;

    s->iommufd_state.devid = 0;
    s->iommufd_state.viommu_id = 0;
    s->iommufd_state.parent_hwpt_id = 0;
    s->iommufd_state.enable_iommu = false;
    qemu_add_vm_change_state_handler(amd_viommu_vm_state_change, s);

    s->vf_mmio_page = NULL;

    amd_viommu_init_mmio(s);

    amd_viommu_init_queue(&s->cmd_queue, IOMMU_HW_QUEUE_TYPE_AMD_CMD);
    amd_viommu_init_queue(&s->evt_queue, IOMMU_HW_QUEUE_TYPE_AMD_EVT);
    amd_viommu_init_queue(&s->ppr_queue, IOMMU_HW_QUEUE_TYPE_AMD_PPR);

    s->devtab_size = 0;
    s->devtab_base = 0;
}

static void amd_viommu_sysbus_realize(DeviceState *dev, Error **errp)
{
    AMDVIOMMUState *s = AMD_VIOMMU_DEVICE(dev);
    MachineState *ms = MACHINE(qdev_get_machine());
    X86MachineState *x86ms = X86_MACHINE(ms);
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    PCIBus *iommu_bus;
    uint64_t base_addr;
    PCIDevice *pdev = NULL;
    int ret;

    if (!s->pci_id) {
        error_setg(errp, "PCI device object not provided");
        return;
    }

    ret = pci_qdev_find_device(s->pci_id, &pdev);

    if (ret) {
        error_setg(errp, "Cannot find PCI device '%s'", s->pci_id);
        return;
    }

    if (!object_dynamic_cast(OBJECT(pdev), TYPE_AMD_VIOMMU_PCI)) {
        error_setg(errp, "Device '%s' must be an AMDVI-PCI device type", s->pci_id);
        return;
    }

    iommu_bus = pci_get_bus(pdev);
    s->pci = AMD_VIOMMU_PCI(pdev);
    s->ivrs_info.pci = s->pci;

    base_addr = AMDVI_GET_BASE_ADDR(x86_iommu->index);

    /* Set up PCI Capability base address */
    pci_set_long(s->pci->dev.config + s->pci->capab_offset + AMDVI_CAPAB_BAR_LOW,
                 base_addr & MAKE_64BIT_MASK(14, 18));
    pci_set_long(s->pci->dev.config + s->pci->capab_offset + AMDVI_CAPAB_BAR_HIGH,
                base_addr >> 32);

    s->ivrs_info.root_bus = get_root_bus(iommu_bus);
    s->ivrs_info.base_addr = base_addr;

    /* set up MMIO */
    memory_region_init_io(&s->mr_mmio, OBJECT(s), &mmio_mem_ops, s,
                          "amd-viommu-mmio", AMDVI_MMIO_SIZE);
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

    amd_viommu_init(s);
    pci_setup_iommu(s->ivrs_info.root_bus, &amd_viommu_pci_ops, s);
}

static const Property amd_viommu_properties[] = {
    DEFINE_PROP_STRING("pci-id", AMDVIOMMUState, pci_id),
};

static const VMStateDescription vmstate_amd_viommu_sysbus = {
    .name = "amd-viommu",
    .unmigratable = 1
};

/* Stub call, will be filled later on */
static void amd_viommu_sysbus_reset(DeviceState *dev)
{
    AMDVIOMMUState *s = AMD_VIOMMU_DEVICE(dev);

    amd_viommu_dealloc_devtab(s);
    amd_viommu_reset_device_hwpt_all(s);
}

static void amd_viommu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *dc_class = X86_IOMMU_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, amd_viommu_sysbus_reset);
    dc->vmsd = &vmstate_amd_viommu_sysbus;
    dc->hotpluggable = false;
    dc_class->realize = amd_viommu_sysbus_realize;

    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD VIOMMU device";
    device_class_set_props(dc, amd_viommu_properties);
}

static const TypeInfo amd_viommu = {
    .name = TYPE_AMD_VIOMMU_DEVICE,
    .parent = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(AMDVIOMMUState),
    .class_init = amd_viommu_class_init
};

static void amd_viommu_pci_realize(PCIDevice *pdev, Error **errp)
{
    AMDVIPCIState *s = AMD_VIOMMU_PCI(pdev);
    int ret;

    ret = pci_add_capability(pdev, AMDVI_CAPAB_ID_SEC, 0,
                             AMDVI_CAPAB_SIZE, errp);
    if (ret < 0) {
        error_setg(errp, "pci capability AMDVI_CAPAB_ID_SEC add failed");
        return;
    }
    s->capab_offset = ret;

    ret = pci_add_capability(pdev, PCI_CAP_ID_HT, 0,
                             AMDVI_CAPAB_REG_SIZE, errp);
    if (ret < 0) {
        error_setg(errp, "pci capability PCI_CAP_ID_HT add failed");
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

static void amd_viommu_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = 0x1419;
    k->class_id = 0x0806;
    k->realize = amd_viommu_pci_realize;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD IOMMU (AMD-Vi) DMA Remapping device";
}

static const TypeInfo amd_viommu_pci= {
    .name = "AMD-VIOMMU-PCI",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDVIPCIState),
    .class_init = amd_viommu_pci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

__attribute__((unused)) static void amd_viommu_pci_register_types(void)
{
    type_register_static(&amd_viommu_pci);
    type_register_static(&amd_viommu);
}

type_init(amd_viommu_pci_register_types);
