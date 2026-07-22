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

    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amd_viommu_mmio_write_invalid(AMDVI_MMIO_SIZE, addr, size, val);
        return;
    }

    trace_amd_viommu_mmio_write(amdvi_mmio_get_name(addr), addr, size, val,
                                addr & 0x07);

    amd_viommu_mmio_reg_write(s, size, val, addr);
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
    AMDDeviceKey key = {
        .bus = bus,
        .devfn = devfn,
    };

    if (!g_hash_table_lookup(s->iommufd_state.iommufd_dev_hash, &key)) {
        return;
    }

    trace_amd_viommu_unset_iommu_device(pci_bus_num(bus), PCI_SLOT(devfn),
                                        PCI_FUNC(devfn));

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
        s->iommufd_state.devid = HOST_IOMMU_DEVICE_IOMMUFD(hiod)->devid;
    }

    new_key = g_malloc(sizeof(AMDDeviceKey));
    new_key->bus = bus;
    new_key->devfn = devfn;

    amd_idev = g_malloc0(sizeof(AMDIOMMUFDDevice));
    amd_idev->viommu = s;
    amd_idev->hiod = hiod;

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
    qemu_add_vm_change_state_handler(amd_viommu_vm_state_change, s);

    s->vf_mmio_page = NULL;

    amd_viommu_init_mmio(s);
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
