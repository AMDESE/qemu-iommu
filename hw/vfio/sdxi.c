/*
 * VFIO-based SDXI (Scalable Data Transfer Interface) PCI device
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SDXI devices can be passed through to the guest directly.  All device
 * operation (context management, descriptor rings, DMA) is driven by
 * the guest kernel's SDXI driver.  The host kernel VFIO migration
 * driver (sdxi-vfio-pci) saves and restores the small set of writable
 * BAR0 control registers; everything else lives in guest RAM and is
 * captured as part of the VM memory snapshot.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "pci.h"
#include "qom/object.h"

#define TYPE_VFIO_SDXI "vfio-sdxi"
OBJECT_DECLARE_SIMPLE_TYPE(VFIOSDXIDevice, VFIO_SDXI)

struct VFIOSDXIDevice {
    VFIOPCIDevice parent_obj;
};

static DeviceRealize vfio_sdxi_parent_realize;

static void vfio_sdxi_instance_init(Object *obj)
{
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(obj);

    vdev->vbasedev.enable_migration = ON_OFF_AUTO_ON;
}

static void vfio_sdxi_realize(DeviceState *dev, Error **errp)
{
    PCIDevice *pdev = PCI_DEVICE(dev);
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(dev);
    ERRP_GUARD();

    vfio_sdxi_parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    /*
     * Relax the migration cmask check on every config-space byte that QEMU
     * does not emulate (i.e. is passed through to the physical device).
     * Read-only fields like PCI_EXP_LNKCAP can legitimately differ between
     * source and destination after a device reset, which would otherwise
     * cause get_pci_config_device() to reject the migration stream.
     */
    for (int i = PCI_CONFIG_HEADER_SIZE; i < vdev->config_size; i++) {
        if (!vdev->emulated_config_bits[i]) {
            pdev->cmask[i] = 0;
        }
    }
}

static void vfio_sdxi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "VFIO-based SDXI PCI device assignment";
    dc->hotpluggable = false;

    device_class_set_parent_realize(dc, vfio_sdxi_realize,
                                    &vfio_sdxi_parent_realize);
}

static const TypeInfo vfio_sdxi_dev_info = {
    .name          = TYPE_VFIO_SDXI,
    .parent        = TYPE_VFIO_PCI,
    .instance_size = sizeof(VFIOSDXIDevice),
    .instance_init = vfio_sdxi_instance_init,
    .class_init    = vfio_sdxi_class_init,
};

static void vfio_sdxi_register_types(void)
{
    type_register_static(&vfio_sdxi_dev_info);
}

type_init(vfio_sdxi_register_types)
