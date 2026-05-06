#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/i386/pc.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"

#include "amd_iommu.h"
#include "amd_viommu.h"

/* 
 * TODO: add command line arguments
 */

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
    memory_region_init_io(&s->mr_mmio, OBJECT(s), NULL, s,
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
}

static const Property amd_viommu_properties[] = {
    DEFINE_PROP_STRING("pci-id", AMDVIState, pci_id),
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
