/*
 * QEMU support of AMD HW-assisted Secure VIOMMU
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

#include "qemu/osdep.h"

#include "hw/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/i386/pc.h"
#include "hw/core/iommu.h"
#include "hw/vfio/vfio-common.h"
#include "system/kvm.h"
#include "system/runstate.h"
#include "target/i386/sev.h"

#include "amd_iommu_common.h"
#include "amd_sviommu.h"
#include "trace.h"

static void amd_sviommu_vm_state_change(void *opaque,
                                        bool running, RunState state);

/* ------------- IOCTL helpers  -------------*/

/*
 * Get hwpt_id of the device and use it to for VIOMMU_INIT call.
 * This is fine as vfio allocated hwpt with NEST_PARENT flag.
 */
static uint32_t amd_sviommu_get_hwpt_id(AMDSVIOMMUDevice *dev)
{
    VFIODevice *vdev;

    if (!dev || !dev->hiod || !dev->hiod->agent)
        return 0;

    vdev = dev->hiod->agent;
    if (!vdev->hwpt || !vdev->hwpt->hwpt_id) {
        return 0;
    }

    return vdev->hwpt->hwpt_id;
}

static int amd_sviommu_iommu_init(AMDSVIOMMUState *s, AMDSVIOMMUDevice *amd_idev)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    PCIDevice *pdev = &s->pci->dev;
    int viommu_devid = PCI_BUILD_BDF(pci_bus_num(pci_get_bus(pdev)),
                                     pdev->devfn);
    Error *local_err = NULL;

    s->iommufd_viommu_amd.viommu_devid = viommu_devid;
    s->iommufd_viommu_amd.kvmfd = kvm_vmfd(kvm_state);
    /* Host will use this flag to detect secure vIOMMU and does PSP calls */
    s->iommufd_viommu_amd.features = AMD_VIOMMU_FEATURE_SVIOMMU;

    if (!iommufd_backend_alloc_viommu(s->iommufd, idev->devid,
                                      IOMMU_VIOMMU_TYPE_AMD,
                                      s->viommu.s2_hwpt_id,
                                      &s->iommufd_viommu_amd,
                                      sizeof(s->iommufd_viommu_amd),
                                      &s->viommu.viommu_id, &local_err)) {
        error_reportf_err(local_err, "svIOMMU init failed: ");
        return -EINVAL;
    }
    s->viommu.iommufd = s->iommufd;

    trace_amd_sviommu_init(viommu_devid, s->iommufd_viommu_amd.features,
                           s->viommu.s2_hwpt_id);

    return 0;
}

static void amd_sviommu_free_vdev(gpointer key, gpointer value,
                                  gpointer user_data)
{
    AMDSVIOMMUDevice *amd_idev = value;
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

    if (idev->vdevice && idev->vdevice->vdevice_id) {
        iommufd_backend_free_id(amd_idev->sviommu->iommufd,
                                idev->vdevice->vdevice_id);
        g_free(idev->vdevice);
        idev->vdevice = NULL;
    }
}

static int amd_sviommu_iommu_uninit(AMDSVIOMMUState *s)
{
    /* Free vdevice */
    g_hash_table_foreach(s->iommufd_dev_hash, amd_sviommu_free_vdev, NULL);

    if (s->viommu.viommu_id) {
        iommufd_backend_free_id(s->iommufd, s->viommu.viommu_id);
        s->viommu.viommu_id = 0;
    }
    s->enabled = false;

    return 0;
}

/* This path is used to configure IOMMU interrupt */
static int amd_sviommu_ext_int_remap(AMDSVIOMMUState *s, uint32_t vcpu,
                                     uint32_t vector)
{
    Error *local_err = NULL;

    if (!s->viommu.viommu_id) {
        error_report("svIOMMU interrupt remap before vIOMMU init: "
                     "vcpu=%u vector=0x%x", vcpu, vector);
        return -EINVAL;
    }

    if (!iommufd_viommu_ext_int_remap(s->iommufd, s->viommu.viommu_id,
                                      IOMMU_VIOMMU_EXT_INT_EVENT,
                                      kvm_vmfd(kvm_state), vcpu, vector,
                                      &local_err)) {
        error_reportf_err(local_err, "svIOMMU interrupt remap failed: ");
        return -EINVAL;
    }

    return 0;
}

static uint64_t amd_sviommu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    trace_amdvi_mmio_read_invalid(AMDVI_MMIO_SIZE, addr, size);
    error_report("MMIO read is not supported: addr=0x%" HWADDR_PRIx ", size=%u",
                 addr, size);
    return -EINVAL;
}

static void amd_sviommu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    unsigned long offset = addr & 0x07;

    trace_amdvi_mmio_write("error: MMIO write is not supported. ",
                           (uint64_t)AMDVI_MMIO_SIZE, size, val, offset);
    error_report("MMIO write is not supported: addr=0x%" HWADDR_PRIx
                 ", val=0x%" PRIx64, addr, val);
}

static const MemoryRegionOps mmio_mem_ops = {
    .read = amd_sviommu_mmio_read,
    .write = amd_sviommu_mmio_write,
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
 * TODO:
 *   Guest MMIO region is not trapped. Hence we may have to
 *   intialize the EFR register
 */
static void amd_sviommu_mmio_init(AMDSVIOMMUState *s, uint64_t addr)
{
    char *name;
    void *ptr;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    memory_region_init_io(&s->mr_mmio, OBJECT(s), &mmio_mem_ops, s,
                          "amd-sviommu-mmio", AMDVI_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), addr, &s->mr_mmio);

    /* MMIO region: 1st 8K */
    name = g_strdup_printf("sviommu_mmio1_%d", x86_iommu->index);
    memory_region_init_ram_guest_memfd(&s->mr_mmio1, OBJECT(s), name,
                                       AMD_SVIOMMU_MMIO1_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        addr + AMD_SVIOMMU_MMIO1_BASE,
                                        &s->mr_mmio1, 1);
    ptr = memory_region_get_ram_ptr(&s->mr_mmio1);
    sev_snp_launch_update_data_iommu(addr + AMD_SVIOMMU_MMIO1_BASE, ptr,
                                     AMD_SVIOMMU_MMIO1_SIZE);
    g_free(name);

    /* MMIO region: 4th 4K (12K - 16K region) */
    name = g_strdup_printf("sviommu_mmio4_%d", x86_iommu->index);
    memory_region_init_ram_guest_memfd(&s->mr_mmio4, OBJECT(s), name,
                                       AMD_SVIOMMU_MMIO4_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        addr + AMD_SVIOMMU_MMIO4_BASE,
                                        &s->mr_mmio4, 1);
    ptr = memory_region_get_ram_ptr(&s->mr_mmio4);
    sev_snp_launch_update_data_iommu(addr + AMD_SVIOMMU_MMIO4_BASE, ptr,
                                     AMD_SVIOMMU_MMIO4_SIZE);
    g_free(name);
}

/*
 * In secure vIOMMU configurations, acceleration is limited to the third 4K page
 * of the MMIO region. All other regions are treated as no-op. The guest
 * communicates directly with the PSP to program relavent registers (like buffer
 * base addr).
 */
static int amd_sviommu_mmap_mmio(AMDSVIOMMUState *s)
{
    char *name;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    /* MMIO region: 3rd 4K GPA address */
    uint64_t addr = s->ivrs_info.base_addr + AMD_SVIOMMU_VF_MMIO_BASE;
    Error *local_err = NULL;

    /* MMAP VF MMIO space */
    if (!iommufd_backend_viommu_mmap(s->iommufd, s->viommu.viommu_id,
                                     AMDVI_PAGE_SIZE,
                                     s->iommufd_viommu_amd.out_vfmmio_mmap_offset,
                                     &s->mmio_page3, &local_err)) {
        error_reportf_err(local_err, "Failed to mmap VF MMIO: ");
        s->mmio_page3 = NULL;
        return -EIO;
    }

    /*
     * Register the iommufd-mmap'd VF MMIO page as a RAM device region with an
     * associated KVM guest_memfd. The guest_memfd is required so that
     * gmem_set_shareability() can invoke KVM_SET_MEMORY_ATTRIBUTES2 on the fd.
     */
    name = g_strdup_printf("sviommu_mmio3_%d", x86_iommu->index);
    if (!memory_region_init_ram_guest_memfd_device_ptr(
                &s->mr_mmio3, OBJECT(s), name, AMD_SVIOMMU_VF_MMIO_SIZE,
                s->mmio_page3, &error_fatal)) {
        error_report("sviommu: failed to create guest_memfd for VF MMIO page");
        g_free(name);
        return -EIO;
    }
    /* The VF MMIO page is not used for DMA */
    memory_region_set_skip_iommu_map(&s->mr_mmio3, true);
    memory_region_add_subregion_overlap(get_system_memory(), addr,
                                        &s->mr_mmio3, 1);

    kvm_set_memory_attributes_private(addr, AMD_SVIOMMU_VF_MMIO_SIZE);
    g_free(name);

    return 0;
}

static void amd_sviommu_munmap_mmio(AMDSVIOMMUState *s)
{
    if (!s->mmio_page3)
        return;

    munmap(s->mmio_page3, AMDVI_PAGE_SIZE);
    s->mmio_page3 = NULL;
}

/*
 * iommufd_cdev_autodomains_get() allocated domain with NEST_PARENT flag. We can
 * attach viommu to same domain. Hence no need to allocate new domain here
 */
static bool amdvi_setup_sviommu(AMDSVIOMMUState *s, AMDSVIOMMUDevice *amd_idev)
{
    if (s->enabled)
        return true;

    if (amd_sviommu_iommu_init(s, amd_idev))
        goto out;

    if (amd_sviommu_mmap_mmio(s))
        goto out_viommu;

    s->enabled = true;
    return true;

out_viommu:
    amd_sviommu_iommu_uninit(s);

out:
    error_report("Failed to initialize svIOMMU instance. Exiting\n");
    exit (1);
}

static void amdvi_alloc_vdev(gpointer key, gpointer value, gpointer user_data)
{
    AMDSVIOMMUDevice *amd_idev = value;
    AMDSVIOMMUState *s = amd_idev->sviommu;
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    PCIDevice *bridge = amd_idev->bus->parent_dev;
    Error *local_err = NULL;
    uint32_t vdevice_id;

    /* TODO: This logic works for now, need careful review */
    if (bridge && object_dynamic_cast(OBJECT(bridge), TYPE_PCI_BRIDGE))
        amd_idev->gdevid = pci_requester_id(bridge) + 0x100;
    else
        amd_idev->gdevid = PCI_BUILD_BDF(pci_bus_num(amd_idev->bus),
                                         amd_idev->devfn);

    if (!iommufd_backend_alloc_vdev(s->iommufd, idev->devid,
                                    s->viommu.viommu_id, amd_idev->gdevid,
                                    &vdevice_id, &local_err)) {
        error_reportf_err(local_err, "Failed to allocate a vDEVICE: ");
        exit (1);
    }

    idev->vdevice = g_new0(IOMMUFDVdev, 1);
    idev->vdevice->vdevice_id = vdevice_id;
    idev->vdevice->virt_id = amd_idev->gdevid;
    idev->vdevice->idev = idev;
    idev->vdevice->viommu = &s->viommu;

    trace_amd_sviommu_alloc_vdev(amd_idev->gdevid, vdevice_id);
}

static void amdvi_setup_vdevice(AMDSVIOMMUState *s)
{
    g_hash_table_foreach(s->iommufd_dev_hash, amdvi_alloc_vdev, NULL);
}

static bool amd_sviommu_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                         HostIOMMUDevice *hiod, Error **errp)
{
    AMDSVIOMMUState *s = opaque;
    AMDSVIOMMUDevice *amd_idev;
    AMDDeviceKey *new_key;
    AMDDeviceKey key = { .bus = bus, .devfn = devfn };
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    VFIODevice *vdev = hiod->agent;

    assert(hiod);
    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (!vdev || !vdev->tee_io) {
        error_report("Cannot attach non-tee device to svIOMMU\n");
        error_report("Please add x-tio=true for device which is managed by svIOMMU\n");
        exit (1);
    }

    if (g_hash_table_lookup(s->iommufd_dev_hash, &key)) {
        error_setg(errp, "Host IOMMU device already exist");
        return false;
    }

    if (hiod->caps.type != IOMMU_HW_INFO_TYPE_AMD) {
        error_setg(errp, "IOMMU hardware is not compatible");
        return false;
    }

    new_key = g_malloc(sizeof(*new_key));
    new_key->bus = bus;
    new_key->devfn = devfn;

    object_ref(hiod);

    amd_idev = g_malloc0(sizeof(AMDSVIOMMUDevice));
    amd_idev->sviommu = s;
    amd_idev->hiod = hiod;
    amd_idev->bus = bus;
    amd_idev->devfn = devfn;

    g_hash_table_insert(s->iommufd_dev_hash, new_key, amd_idev);

    /* Use iommufd handler opened by device */
    s->iommufd = idev->iommufd;
    if (!s->hwinfo) {
        s->hwinfo = g_memdup2(&hiod->caps.vendor_caps.amd,
                              sizeof(*s->hwinfo));
    }

    trace_amd_sviommu_set_device(s->hwinfo->efr, s->hwinfo->efr2);

    return true;
}

static void amd_sviommu_unset_iommu_device(PCIBus *bus, void *opaque, int devfn)
{
    AMDSVIOMMUState *s = opaque;
    AMDDeviceKey key = { .bus = bus, .devfn = devfn };
    AMDSVIOMMUDevice *amd_idev;

    amd_idev = g_hash_table_lookup(s->iommufd_dev_hash, &key);
    if (!amd_idev) {
        return;
    }

    amd_sviommu_free_vdev(&key, amd_idev, NULL);
    object_unref(amd_idev->hiod);
    g_hash_table_remove(s->iommufd_dev_hash, &key);
}

static AddressSpace *amd_sviommu_get_address_space(PCIBus *bus, void *opaque,
                                                   int devfn)
{
    return &address_space_memory;
}

static int amd_sviommu_get_x86_iommu(void *opaque, void **x86_iommu)
{
    AMDSVIOMMUState *s = opaque;
    *x86_iommu = X86_IOMMU_DEVICE(s);
    return 0;
}

static uint64_t amd_sviommu_get_viommu_flags(void *opaque)
{
    return VIOMMU_FLAG_WANT_NESTING_PARENT;
}

static void amd_sviommu_state_change(AMDSVIOMMUState *s)
{
    GHashTableIter as_it;
    AMDDeviceKey *key;
    AMDSVIOMMUDevice *amd_idev;
    uint32_t hwpt_id = 0;

    if (s->enabled)
        return;

    /* Try to get hwpt_id from devices behind this sviommu instance and
     * initialize the vIOMMU instance
     */
    g_hash_table_iter_init(&as_it, s->iommufd_dev_hash);

    while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
        hwpt_id = amd_sviommu_get_hwpt_id(amd_idev);
        if (hwpt_id == 0)
            continue;

        s->viommu.s2_hwpt_id = hwpt_id;
        amdvi_setup_sviommu(s, amd_idev);
        amdvi_setup_vdevice(s);
        break;
    }

    if (s->viommu.s2_hwpt_id == 0) {
        error_report("Cannot initialize svIOMMU without device. Exiting\n");
        exit (1);
    }
}

static void amd_sviommu_vm_state_change(void *opaque,
                                        bool running, RunState state)
{
    AMDSVIOMMUState *s = opaque;

    switch (state) {
    case RUN_STATE_RUNNING:
        amd_sviommu_state_change(s);
        break;
    case RUN_STATE_SHUTDOWN:
        if (!s->mmio_page3)
            break;

        amd_sviommu_munmap_mmio(s);
        amd_sviommu_iommu_uninit(s);
        break;
    default:
        break;
    }
}

static const PCIIOMMUOps amd_sviommu_pci_ops = {
    .get_address_space = amd_sviommu_get_address_space,
    .set_iommu_device = amd_sviommu_set_iommu_device,
    .unset_iommu_device = amd_sviommu_unset_iommu_device,
    .get_x86_iommu = amd_sviommu_get_x86_iommu,
    .get_viommu_flags = amd_sviommu_get_viommu_flags,
};

static void amd_sviommu_unrealize(DeviceState *dev)
{
    AMDSVIOMMUState *s = AMD_SVIOMMU_DEVICE(dev);

    if (!s->mmio_page3)
        return;

    amd_sviommu_munmap_mmio(s);
    amd_sviommu_iommu_uninit(s);
}

/*
 * TODO:
 *  Currently svIOMMU model (same as vIOMMU model) needs at least one VFIO PCI
 *  device. Otherwise it will not be able to setup IOMMU page table.
 *
 *  Need a way to setup VFIO memory listeners so that it can support sviommu
 *  without devices. But this needs iommufd layer changes as
 *  iommufd_viommu_alloc_ioctl() expect device as param.
 */
static void amd_sviommu_init(AMDSVIOMMUState *s)
{
    s->iommufd_dev_hash = g_hash_table_new_full(amd_iommu_device_key_fn,
                                                amd_iommu_device_key_equal,
                                                g_free, g_free);
    s->hwinfo = NULL;
    s->enabled = false;
    s->mmio_page3 = NULL;

    /*
     * For SEV-TIO guest, we need to do this after klass->launch_finish()
     */
    qemu_add_vm_change_state_handler(amd_sviommu_vm_state_change, s);
}

static void amd_sviommu_sysbus_realize(DeviceState *dev, Error **errp)
{
    AMDSVIOMMUState *s = AMD_SVIOMMU_DEVICE(dev);
    X86MachineState *x86ms = X86_MACHINE(qdev_get_machine());
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

    if (!object_dynamic_cast(OBJECT(pdev), TYPE_AMD_SVIOMMU_PCI)) {
        error_setg(errp, "Device '%s' must be an %s device type", s->pci_id,
                   TYPE_AMD_SVIOMMU_PCI);
        return;
    }

    iommu_bus = pci_get_bus(pdev);
    s->pci = AMD_SVIOMMU_PCI(pdev);
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
    amd_sviommu_mmio_init(s, base_addr);

    /*
     * The linux kernel disables the intremap when it cannot find IOAPIC under
     * AMD IOMMU IVRS.
     */
    if (!x86ms->ioapic_iommu && x86_iommu_ir_supported(x86_iommu)) {
        x86ms->ioapic_iommu = x86_iommu;
    }

    amd_sviommu_init(s);
    pci_setup_iommu(s->ivrs_info.root_bus, &amd_sviommu_pci_ops, s);
}

static const Property amd_sviommu_properties[] = {
    DEFINE_PROP_STRING("pci-id", AMDSVIOMMUState, pci_id),
};

static const VMStateDescription vmstate_amd_sviommu = {
    .name = "amd-sviommu",
    .unmigratable = 1
};

static void amd_sviommu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *dc_class = X86_IOMMU_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_amd_sviommu;
    device_class_set_props(dc, amd_sviommu_properties);
    dc->hotpluggable = false;
    dc_class->realize = amd_sviommu_sysbus_realize;
    dc->unrealize = amd_sviommu_unrealize;

    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD SVIOMMU device";
}

static const TypeInfo amd_sviommu = {
    .name = TYPE_AMD_SVIOMMU_DEVICE,
    .parent = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(AMDSVIOMMUState),
    .class_init = amd_sviommu_class_init
};

static AMDSVIOMMUState *get_sviommu(PCIDevice *pdev)
{
    X86IOMMUState *iommu;

    QLIST_FOREACH(iommu, x86_iommu_get_list_head(), next) {
        AMDSVIOMMUState *s;

        if (!object_dynamic_cast(OBJECT(iommu), TYPE_AMD_SVIOMMU_DEVICE)) {
            continue;
        }

        s = AMD_SVIOMMU_DEVICE(iommu);
        if (s->pci && &s->pci->dev == pdev) {
            return s;
        }
    }

    return NULL;
}

static void amd_sviommu_pci_config_write(PCIDevice *pdev,
                                         uint32_t addr, uint32_t val, int len)
{
    MSIMessage msg = {};
    AMDSVIOMMUState *s;
    int vcpu, vector;

    pci_default_write_config(pdev, addr, val, len);

    /* TODO: Find better way to identify MSI block inside config space */
    if (val == 0)
        return;

    /* Extract vCPU and vector from MSI msg */
    msg = msi_get_message(pdev, 0);
    vcpu = (msg.address >> 12) & 0xFF;
    vector = msg.data & 0xFF;

    if (vector == 0)
        return;

    /* Get svIOMMU instance from its PCI device */
    s = get_sviommu(pdev);
    if (!s)
        return;

    trace_amd_sviommu_msi_interrupt(vcpu, vector);

    amd_sviommu_ext_int_remap(s, vcpu, vector);
}

static void amd_sviommu_pci_realize(PCIDevice *pdev, Error **errp)
{
    AMDVIPCIState *s = AMD_SVIOMMU_PCI(pdev);
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

    /* reset device ident */
    pci_config_set_prog_interface(pdev->config, 0);

    /* reset AMDVI specific capabilities, all r/o */
    pci_set_long(pdev->config + s->capab_offset, AMD_SVIOMMU_CAPAB_FEATURES);
    pci_set_long(pdev->config + s->capab_offset + AMDVI_CAPAB_RANGE,
                 0xff000000);
    pci_set_long(pdev->config + s->capab_offset + AMDVI_CAPAB_MISC, 0);
    pci_set_long(pdev->config + s->capab_offset + AMDVI_CAPAB_MISC,
                 AMDVI_MAX_PH_ADDR | AMDVI_MAX_GVA_ADDR | AMDVI_MAX_VA_ADDR);

    msi_init(pdev, 0, 1, true, false, errp);
}

/* Add PCI config write callback so that we can capture MSI config space */
static void amd_sviommu_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_AMD;
    k->device_id = 0x1419;
    k->class_id = 0x0806;
    k->realize = amd_sviommu_pci_realize;
    k->config_write = amd_sviommu_pci_config_write;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD Secure IOMMU (AMD-Vi) DMA Remapping device";
}

static const TypeInfo amd_sviommu_pci = {
    .name = TYPE_AMD_SVIOMMU_PCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDVIPCIState),
    .class_init = amd_sviommu_pci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void amd_sviommu_pci_register_types(void)
{
    type_register_static(&amd_sviommu_pci);
    type_register_static(&amd_sviommu);
}

type_init(amd_sviommu_pci_register_types);
