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

#include "amd_iommu.h"

void amdvi_set_quad(AMDVIState *s, hwaddr addr, uint64_t val,
                           uint64_t romask, uint64_t w1cmask)
{
    stq_le_p(&s->mmior[addr], val);
    stq_le_p(&s->romask[addr], romask);
    stq_le_p(&s->w1cmask[addr], w1cmask);
}



uint16_t amdvi_readw(AMDVIState *s, hwaddr addr)
{
    return lduw_le_p(&s->mmior[addr]);
}



uint32_t amdvi_readl(AMDVIState *s, hwaddr addr)
{
    return ldl_le_p(&s->mmior[addr]);
}



uint64_t amdvi_readq(AMDVIState *s, hwaddr addr)
{
    return ldq_le_p(&s->mmior[addr]);
}



void amdvi_writew(AMDVIState *s, hwaddr addr, uint16_t val)
{
    uint16_t romask = lduw_le_p(&s->romask[addr]);
    uint16_t w1cmask = lduw_le_p(&s->w1cmask[addr]);
    uint16_t oldval = lduw_le_p(&s->mmior[addr]);

    uint16_t oldval_preserved = oldval & (romask | w1cmask);
    uint16_t newval_write = val & ~romask;
    uint16_t newval_w1c_set = val & w1cmask;

    stw_le_p(&s->mmior[addr],
             (oldval_preserved | newval_write) & ~newval_w1c_set);
}



void amdvi_writel(AMDVIState *s, hwaddr addr, uint32_t val)
{
    uint32_t romask = ldl_le_p(&s->romask[addr]);
    uint32_t w1cmask = ldl_le_p(&s->w1cmask[addr]);
    uint32_t oldval = ldl_le_p(&s->mmior[addr]);

    uint32_t oldval_preserved = oldval & (romask | w1cmask);
    uint32_t newval_write = val & ~romask;
    uint32_t newval_w1c_set = val & w1cmask;

    stl_le_p(&s->mmior[addr],
             (oldval_preserved | newval_write) & ~newval_w1c_set);
}



void amdvi_writeq(AMDVIState *s, hwaddr addr, uint64_t val)
{
    uint64_t romask = ldq_le_p(&s->romask[addr]);
    uint64_t w1cmask = ldq_le_p(&s->w1cmask[addr]);
    uint64_t oldval = ldq_le_p(&s->mmior[addr]);

    uint64_t oldval_preserved = oldval & (romask | w1cmask);
    uint64_t newval_write = val & ~romask;
    uint64_t newval_w1c_set = val & w1cmask;

    stq_le_p(&s->mmior[addr],
             (oldval_preserved | newval_write) & ~newval_w1c_set);
}



void amdvi_mmio_reg_write(AMDVIState *s, unsigned size, uint64_t val,
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


