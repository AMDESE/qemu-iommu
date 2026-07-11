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

#include <sys/ioctl.h>
#include <linux/amd_viommu.h>

#include "qemu/osdep.h"
#include "hw/i386/pc.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
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
#include "system/kvm.h"

#include "target/i386/sev.h"
#include "system/runstate.h"
#include "system/iommufd.h"

struct amd_as_key {
    PCIBus *bus;
    uint8_t devfn;
};

static void amd_sviommu_vm_state_change(void *opaque,
                                        bool running, RunState state);

/* ------------- IOCTL helpers  -------------*/

/*
 * Get hwpt_id of the device and use it to for VIOMMU_INIT call.
 * This is fine as vfio allocated hwpt with NEST_PARENT flag.
 */
static uint32_t amd_sviommu_get_hwpt_id(AMDIOMMUFDDevice *dev)
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

#if 0
/*
 * Find another sviommu instance that has a device behind it and return its
 * hwpt_id. Since GPA -> SPA mapping is the same for all sviommu instances
 * in a guest, we can use hwpt_id from any instance that has a device.
 */
static uint32_t amd_sviommu_find_hwpt_id_from_other_instance(AMDVIState *update_s)
{
    X86IOMMUState *iommu;
    AMDVIState *s;
    struct amd_as_key *key;
    AMDIOMMUFDDevice *amd_idev;
    HostIOMMUDeviceHwInfo hwinfo;
    HostIOMMUDeviceIOMMUFD *idev;
    GHashTableIter as_it;
    uint32_t hwpt_id;

    QLIST_FOREACH(iommu, x86_iommu_get_iommu_list_head(), next) {
        if (!object_dynamic_cast(OBJECT(iommu), TYPE_AMD_SVIOMMU_DEVICE))
            continue;

        s = AMD_SVIOMMU_DEVICE(iommu);
        if (s == update_s || !s->amd_iommufd_dev_hash)
            continue;

        /* Find first device in this instance with valid hwpt_id */
        g_hash_table_iter_init(&as_it, s->amd_iommufd_dev_hash);
        while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
            hwpt_id = amd_sviommu_get_hwpt_id(amd_idev);
            if (hwpt_id == 0)
                continue;

            hwinfo = amd_idev->hiod->hwinfo;
	    idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

            /*
	     * Note:
	     *   Ideally each IOMMU should call IOMMU_GET_HW_INFO to get the
	     *   EFR. We know that all IOMMU EFRs are same. Hence when there
	     *   is no device behind the svIOMMU instance then copy EFR from
	     *   other IOMMU instance.
	     */
	    update_s->iommufd = idev->iommufd;
            update_s->hwinfo.efr = hwinfo.amd.efr;
            update_s->hwinfo.efr2 = hwinfo.amd.efr2;

            return hwpt_id;
        }
    }

    return 0;
}
#endif

static int amd_sviommu_iommu_init(AMDVIState *s, AMDIOMMUFDDevice *amd_idev)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
    PCIDevice *pdev = &s->pci.dev;
    int viommu_devid = pci_bus_num(pci_get_bus(pdev)) | pdev->devfn;

    //s->iommufd_viommu_amd.iommu_devid = bdf;
    //s->iommufd_viommu_amd.trans_devid = s->translate_id;
    s->iommufd_viommu_amd.viommu_devid = viommu_devid;
    s->iommufd_viommu_amd.kvmfd = kvm_vmfd(kvm_state);
    /* Host will use this flag to detect secure vIOMMU and does PSP calls */
    s->iommufd_viommu_amd.features = AMD_VIOMMU_FEATURE_SVIOMMU;

    s->core = iommufd_backend_alloc_viommu(s->iommufd, idev->devid,
					   IOMMU_VIOMMU_TYPE_AMD,
					   s->parent_hwpt_id,
					   sizeof(s->iommufd_viommu_amd),
					   &s->iommufd_viommu_amd);

    if (!s->core) {
        error_report("svIOMMU init failed: %s", strerror(errno));
        return -EINVAL;
    }

    trace_amd_sviommu_init(viommu_devid, s->iommufd_viommu_amd.features, s->parent_hwpt_id);

    //s->gid = s->iommufd_viommu_amd.out_gid;
    fprintf(stderr, "DEBUG: %s: secure vIOMMU initialied. viommu_devid=%#x\n",
	    __func__, viommu_devid);

    return 0;
}

static int amd_sviommu_iommu_uninit(AMDVIState *s)
{
    struct amd_as_key *key;
    AMDIOMMUFDDevice *amd_idev;
    GHashTableIter as_it;

    /* Free vdevice */
    g_hash_table_iter_init(&as_it, s->amd_iommufd_dev_hash);
    while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
        HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
        if (idev->vdevice && idev->vdevice->vdev_id) {
            iommufd_backend_free_id(s->iommufd, idev->vdevice->vdev_id);
        }
    }

    if (s->enabled) {
        iommufd_backend_free_id(s->iommufd, s->core->viommu_id);
        s->enabled = false;
    }

    return 0;
}

/* This path is used to configure IOMMU interrupt */
static int amd_sviommu_mmio_write(AMDVIState *s, __u32 offset,
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

    ret = ioctl(s->iommufd->fd, VIOMMU_MMIO_ACCESS, &arg);
    if (ret) {
        error_report("svIOMMU MMIO write failed: offset=0x%x, value=0x%llx, %s",
                     offset, value, strerror(errno));
    }

    return ret;
}

/* TODO: Do we need this? */
static AddressSpace *amd_sviommu_host_dma_iommu(PCIBus *bus, void *opaque, int devfn)
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
                                 TYPE_AMD_SVIOMMU_MEMORY_REGION,
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

static uint64_t amdvi_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    trace_amdvi_mmio_read_invalid(AMD_VIOMMU_MMIO_SIZE, addr, size);
    error_report("MMIO read is not supported: addr=0x%" HWADDR_PRIx ", size=%u",
                 addr, size);
    return -EINVAL;
}

static void amdvi_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    unsigned long offset = addr & 0x07;

    trace_amdvi_mmio_write("error: MMIO write is not supported. ",
                           (uint64_t)AMD_VIOMMU_MMIO_SIZE, size, val, offset);
    error_report("MMIO write is not supported: addr=0x%" HWADDR_PRIx
                 ", val=0x%" PRIx64, addr, val);
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
 * TODO:
 *  Currently svIOMMU model (same as vIOMMU model) needs at least one VFIO PCI
 *  device. Otherwise it will not be able to setup IOMMU page table.
 *
 *  Need a way to setup VFIO memory listeners so that it can support sviommu
 *  without devices. But this needs iommufd layer changes as
 *  iommufd_viommu_alloc_ioctl() expect device as param.
 */
static void amd_sviommu_init(AMDVIState *s)
{
    s->enabled = false;
    s->ats_enabled = false;

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
    pci_set_long(s->pci.dev.config + s->pci.capab_offset,
                 AMDVI_CAPAB_FEATURES | AMDVI_CAPAB_FLAG_NPCACHE );
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
    qemu_add_vm_change_state_handler(amd_sviommu_vm_state_change, s);
}

/*
 * TODO:
 *   Guest MMIO region is not trapped. Hence we may have to
 *   intialize the EFR register
 */
static void amd_sviommu_mmio_init(AMDVIState *s)
{
    char *name;
    void *ptr;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint64_t addr = AMDVI_BASE_ADDR + (x86_iommu->index * AMD_VIOMMU_MMIO_SIZE);

    memory_region_init_io(&s->mr_mmio, OBJECT(s), &mmio_mem_ops, s, "amdvi-svmmio",
                          AMD_VIOMMU_MMIO_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mr_mmio);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, addr);

    /* MMIO region: 1st 8K */
    name = g_strdup_printf("sviommu_mmio1_%d", x86_iommu->index);
    memory_region_init_ram_guest_memfd(&s->mr_mmio1, memory_region_owner(&s->mr_mmio1),
                                       name, 0x2000, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), addr, &s->mr_mmio1, 1);
    ptr = memory_region_get_ram_ptr(&s->mr_mmio1);
#if 0
    /* Write known pattern to verify */
    memset(ptr, 0, 0x2000);
#endif
    sev_snp_launch_update_data_iommu(addr, ptr, 0x2000);
    g_free(name);

    /* MMIO region: 4th 4K (12K - 16K region) */
    name = g_strdup_printf("sviommu_mmio4_%d", x86_iommu->index);
    memory_region_init_ram_guest_memfd(&s->mr_mmio4, memory_region_owner(&s->mr_mmio4),
                                       name, 0x1000, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), addr + 0x3000,
		                        &s->mr_mmio4, 1);

    ptr = memory_region_get_ram_ptr(&s->mr_mmio4);
    sev_snp_launch_update_data_iommu(addr + 0x3000, ptr, 0x1000);
    g_free(name);
}

/*
 * In secure vIOMMU configurations, acceleration is limited to the third 4K page
 * of the MMIO region. All other regions are treated as no-op. The guest
 * communicates directly with the PSP to program relavent registers (like buffer
 * base addr).
 */
static int amd_sviommu_mmap_mmio(AMDVIState *s)
{
    char *name;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    /* MMIO region: 3rd 4K GPA address */
    uint64_t addr = AMDVI_BASE_ADDR + (x86_iommu->index * AMD_VIOMMU_MMIO_SIZE) + 0x2000;

    /* MMAP VF MMIO space */
    s->mmio_page3 = mmap(NULL, AMDVI_PAGE_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED, s->iommufd->fd,
			 s->iommufd_viommu_amd.out_vfmmio_mmap_offset);
    if (s->mmio_page3 == MAP_FAILED) {
        error_report("Failed to mmap VF MMIO: %s", strerror(errno));
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
                &s->mr_mmio3, memory_region_owner(&s->mr_mmio3),
                name, 0x1000, s->mmio_page3, &error_fatal)) {
        error_report("sviommu: failed to create guest_memfd for VF MMIO page");
        g_free(name);
        return -EIO;
    }
    memory_region_add_subregion_overlap(get_system_memory(), addr,
                                        &s->mr_mmio3, 1);

    kvm_set_memory_attributes_private(addr, 0x1000);
    g_free(name);

    return 0;
}

/*
 * iommufd_cdev_autodomains_get() allocated domain with NEST_PARENT flag. We can
 * attach viommu to same domain. Hence no need to allocate new domain here
 */
static bool amdvi_setup_sviommu(AMDVIState *s, AMDIOMMUFDDevice *amd_idev)
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

/* PCIIOMMUOps::config_iommu_device callback. */
static bool amdvi_setup_vdevice(PCIBus *bus, void *opaque, int devfn,
                                Error **errp)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *amd_idev;
    HostIOMMUDeviceIOMMUFD *idev;
    struct amd_as_key key = {
        .bus = bus,
        .devfn = devfn,
    };

    amd_idev = g_hash_table_lookup(s->amd_iommufd_dev_hash, &key);
    if (!amd_idev) {
        error_setg(errp, "No host IOMMU device for %02x:%02x.%x",
                   pci_bus_num(bus), PCI_SLOT(devfn), PCI_FUNC(devfn));
        return false;
    }

    idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

    /* vDEVICE already allocated for this device, nothing to do */
    if (idev->vdevice) {
        return true;
    }

    /* The svIOMMU (s->core) must be initialized before allocating a vDEVICE */
    if (!s->core) {
        error_setg(errp, "svIOMMU instance is not initialized yet");
        return false;
    }

    amd_idev->gdevid = PCI_BUILD_BDF(pci_bus_num(amd_idev->bus),
                                     amd_idev->devfn);

    fprintf(stderr, "DEBUG: %s gdevid=0x%x\n", __func__, amd_idev->gdevid);

    idev->vdevice = iommufd_backend_alloc_vdev(idev, s->core,
                                               amd_idev->gdevid);
    if (!idev->vdevice) {
        error_setg(errp, "Failed to allocate a vDEVICE (gdevid=0x%x)",
                   amd_idev->gdevid);
        return false;
    }

    return true;
}

static bool amdvi_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                   HostIOMMUDevice *hiod, Error **errp)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *amd_idev;
    struct amd_as_key *new_key;
    struct amd_as_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    HostIOMMUDeviceHwInfo hwinfo = hiod->hwinfo;
    VFIODevice *vdev = hiod->agent;

    assert(hiod);
    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (!vdev || !vdev->tee_io) {
        error_report("Cannot attach non-tee device to svIOMMU\n");
        error_report("Please add x-tio=true for device which is managed by svIOMMU\n");
        exit (1);
    }

    /* Check for duplicate using amd_iommufd_dev_hash */
    if (g_hash_table_lookup(s->amd_iommufd_dev_hash, &key)) {
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

    amd_idev = g_malloc0(sizeof(AMDIOMMUFDDevice));
    amd_idev->iommu_state = s;
    amd_idev->hiod = hiod;
    amd_idev->bus = bus;
    amd_idev->devfn = devfn;

    g_hash_table_insert(s->amd_iommufd_dev_hash, new_key, amd_idev);

    /* Use iommufd handler opened by device */
    s->iommufd = idev->iommufd;
    s->hwinfo.efr = hwinfo.amd.efr;
    s->hwinfo.efr2 = hwinfo.amd.efr2;

    trace_amd_sviommu_set_device(hwinfo.amd.efr, hwinfo.amd.efr2);

    return true;
}

static void amdvi_unset_iommu_device(PCIBus *bus, void *opaque, int devfn)
{
    AMDVIState *s = opaque;
    struct amd_as_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    AMDIOMMUFDDevice *amd_idev;

    amd_idev = g_hash_table_lookup(s->amd_iommufd_dev_hash, &key);
    if (!amd_idev) {
        return;
    }

    object_unref(amd_idev->hiod);
    g_hash_table_remove(s->amd_iommufd_dev_hash, &key);
}

static void amd_sviommu_state_change(AMDVIState *s)
{
    struct amd_as_key *key;
    AMDIOMMUFDDevice *amd_idev;
    GHashTableIter as_it;
    uint32_t hwpt_id = 0;

    if (s->enabled)
        return;

    /* Try to get hwpt_id from devices behind this sviommu instance and
     * initialize the vIOMMU instance
     */
    g_hash_table_iter_init(&as_it, s->amd_iommufd_dev_hash);

    while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
        hwpt_id = amd_sviommu_get_hwpt_id(amd_idev);
        if (hwpt_id == 0)
            continue;

        s->parent_hwpt_id = hwpt_id;
        amdvi_setup_sviommu(s, amd_idev);
        break;
    }

    if (s->parent_hwpt_id == 0) {
        error_report("Cannot initialize svIOMMU without device. Exiting\n");
        exit (1);
    }

#if 0
    /*
     * If no device in this instance, find hwpt_id from another sviommu
     * instance and use it for viommu_init call. This is fine as GPA -> SPA
     * is same for the given guest.
     */
    if (hwpt_id == 0)
        hwpt_id = amd_sviommu_find_hwpt_id_from_other_instance(s);
#endif
}

static void amd_sviommu_vm_state_change(void *opaque,
                                        bool running, RunState state)
{
    AMDVIState *s = opaque;

    switch (state) {
    case RUN_STATE_RUNNING:
        amd_sviommu_state_change(s);
        break;
    case RUN_STATE_SHUTDOWN:
        if (!s->mmio_page3)
            break;

        munmap(s->mmio_page3, AMDVI_PAGE_SIZE);
        s->mmio_page3 = NULL;
        amd_sviommu_iommu_uninit(s);
        break;
    default:
        break;
    }
}

static PCIIOMMUOps amdvi_iommu_ops = {
    .set_iommu_device = amdvi_set_iommu_device,
    .unset_iommu_device = amdvi_unset_iommu_device,
    .config_iommu_device = amdvi_setup_vdevice,
    .get_address_space = NULL,
};

static void amd_sviommu_unrealize(DeviceState *dev)
{
    AMDVIState *s = AMD_SVIOMMU_DEVICE(dev);

    if (!s->mmio_page3)
        return;

    munmap(s->mmio_page3, AMDVI_PAGE_SIZE);
    s->mmio_page3 = NULL;

    amd_sviommu_iommu_uninit(s);
}

static gboolean amd_as_equal(gconstpointer v1, gconstpointer v2)
{
    const struct amd_as_key *key1 = v1;
    const struct amd_as_key *key2 = v2;

    return (key1->bus == key2->bus) && (key1->devfn == key2->devfn);
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

static void amd_sviommu_realize(DeviceState *dev, Error **errp)
{
    int ret = 0;
    AMDVIState *s = AMD_SVIOMMU_DEVICE(dev);
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

    /* setup IOMMU PCI device ID in the guest. */
    amd_sviommu_host_dma_iommu(bus, s, s->pci.dev.devfn);

    /* set up MMIO */
    amd_sviommu_mmio_init(s);

    /*
     * Setup vIOMMU for the device w/o specifying the iommu_fn to avoid
     * calling amdvi_host_dma_iommu(). See pci_device_iommu_addres_space()
     * on calling of iommu_fn().
     */
    if (s->primary_bus) {
        bus = s->primary_bus;
    }
    pci_setup_iommu(bus, &amdvi_iommu_ops, s);

    msi_init(&s->pci.dev, 0, 1, true, false, errp);

    amd_sviommu_init(s);

    fprintf(stderr, "DEBUG IOMMU REALIZE\n");
    s->amd_iommufd_dev_hash = g_hash_table_new_full(amd_as_hash, amd_as_equal,
                                                     g_free, g_free);
}

static void amd_sviommu_instance_finalize(Object *obj)
{
    /* Cleanup handled in unrealize */
}

static const VMStateDescription vmstate_amdvi = {
    .name = "amd-sviommu",
    .unmigratable = 1
};

static void amd_sviommu_instance_init(Object *obj)
{
    AMDVIState *s = AMD_SVIOMMU_DEVICE(obj);

    object_initialize(&s->pci, sizeof(s->pci), TYPE_AMD_SVIOMMU_PCI);
}

static const Property amd_sviommu_properties[] = {
    DEFINE_PROP_LINK("iommufd", AMDVIState, iommufd,
                     TYPE_IOMMUFD_BACKEND, IOMMUFDBackend *),
    DEFINE_PROP_LINK("primary-bus", AMDVIState, primary_bus,
                     TYPE_PCIE_BUS, PCIBus *),
    DEFINE_PROP_UINT32("last-bus-nr", AMDVIState, last_bus_nr, 0),
    DEFINE_PROP_UINT32("translate-id", AMDVIState, translate_id, 0),
};

/* TODO: Is there a direct way to get AMDVIState from pdev? */
static AMDVIState *get_sviommu(PCIDevice *pdev)
{
    X86IOMMUState *iommu;
    AMDVIState *s;

    QLIST_FOREACH(iommu, x86_iommu_get_iommu_list_head(), next) {
        PCIDevice *sviommu_pdev;
        uint16_t bdf;

        if (!object_dynamic_cast(OBJECT(iommu), TYPE_AMD_SVIOMMU_DEVICE)) {
            continue;
        }

        s = AMD_SVIOMMU_DEVICE(iommu);
        sviommu_pdev = &s->pci.dev;
        bdf = pci_bus_num(pci_get_bus(sviommu_pdev)) | sviommu_pdev->devfn;

        if (bdf == pdev->devfn) {
            return s;
        }
    }

    return NULL;
}

static void amd_sviommu_pci_config_write(PCIDevice *pdev,
                                        uint32_t addr, uint32_t val, int len)
{
    MSIMessage msg = {};
    AMDVIState *s;
    uint64_t value;
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

    /* Get svIOMMU instance using PCI BDF */
    s = get_sviommu(pdev);
    if (!s)
        return;

    /* Generate MMIO Offset 0x170h XT IOMMU General Interrupt Register value */
    value = ((int64_t)vector << 32) | (vcpu << 8);

    trace_amd_sviommu_msi_interrupt(vcpu, vector, value);

    amd_sviommu_mmio_write(s, AMDVI_MMIO_XT_EVENT_INT, 8, value);
}

/* Add PCI config write callback so that we can capture MSI config space */
static void amd_sviommu_pci_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->config_write = amd_sviommu_pci_config_write;
}

static void amd_sviommu_class_init(ObjectClass *klass, void* data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *dc_class = X86_IOMMU_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_amdvi;
    device_class_set_props(dc, amd_sviommu_properties);
    dc->hotpluggable = false;
    dc_class->realize = amd_sviommu_realize;
    dc_class->unrealize = amd_sviommu_unrealize;

    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "AMD SVIOMMU device";
}

static const TypeInfo AmdSviommu = {
    .name = TYPE_AMD_SVIOMMU_DEVICE,
    .parent = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(AMDVIState),
    .instance_init = amd_sviommu_instance_init,
    .instance_finalize = amd_sviommu_instance_finalize,
    .class_init = amd_sviommu_class_init
};

static const TypeInfo AmdSviommuPCI = {
    .name = "AMD-SVIOMMU-PCI",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AMDVIPCIState),
    .class_init = amd_sviommu_pci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const TypeInfo amd_sviommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_AMD_SVIOMMU_MEMORY_REGION,
};

static void amd_sviommu_pci_register_types(void)
{
    type_register_static(&AmdSviommuPCI);
    type_register_static(&AmdSviommu);
    type_register_static(&amd_sviommu_memory_region_info);
}

type_init(amd_sviommu_pci_register_types);
