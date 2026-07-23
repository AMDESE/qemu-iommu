#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"

#include "amd_iommu_common.h"

/* Go up in the PCI hierarchy and find the root bus pcie.0 or pxb-pci */
PCIBus *get_root_bus(struct PCIBus *bus)
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

guint amd_iommu_device_key_fn(gconstpointer v)
{
    const AMDDeviceKey *key = v;
    guint bus = (guint)(uintptr_t)key->bus;

    return (guint)(bus << 8 | (guint)key->devfn);
}

gboolean amd_iommu_device_key_equal(gconstpointer v1, gconstpointer v2)
{
    const AMDDeviceKey *key1 = v1;
    const AMDDeviceKey *key2 = v2;

    return key1->bus == key2->bus && key1->devfn == key2->devfn;
}
