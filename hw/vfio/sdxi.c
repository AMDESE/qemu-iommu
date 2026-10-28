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
 *
 * Doorbell writes that are swallowed are kept here, migrated with this
 * device, and written back after the function is running again. The
 * device is assumed not to fetch a new descriptor from the write index
 * alone.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "pci.h"
#include "vfio-region.h"
#include "qom/object.h"

#define SDXI_DOORBELL_BAR 2
#define SDXI_MAX_DOORBELLS 128

#define TYPE_VFIO_SDXI "vfio-sdxi"
OBJECT_DECLARE_SIMPLE_TYPE(VFIOSDXIDevice, VFIO_SDXI)

typedef struct SdxiDoorbell {
    uint64_t offset;
    uint64_t value;
} SdxiDoorbell;

struct VFIOSDXIDevice {
    VFIOPCIDevice parent_obj;

    bool swallow_db;
    uint8_t db_region;
    uint32_t nr_db;
    SdxiDoorbell db[SDXI_MAX_DOORBELLS];
};

static const VMStateDescription vmstate_sdxi_doorbell = {
    .name = "vfio-sdxi-doorbell",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(offset, SdxiDoorbell),
        VMSTATE_UINT64(value, SdxiDoorbell),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_vfio_sdxi = {
    .name = "vfio-sdxi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(nr_db, VFIOSDXIDevice),
        VMSTATE_STRUCT_ARRAY(db, VFIOSDXIDevice, SDXI_MAX_DOORBELLS, 0,
                             vmstate_sdxi_doorbell, SdxiDoorbell),
        VMSTATE_END_OF_LIST()
    }
};

static DeviceRealize vfio_sdxi_parent_realize;
static DeviceUnrealize vfio_sdxi_parent_unrealize;

static void vfio_sdxi_note_doorbell(VFIOSDXIDevice *s, uint64_t offset,
                                    uint64_t value)
{
    uint32_t i;

    for (i = 0; i < s->nr_db; i++) {
        if (s->db[i].offset == offset) {
            s->db[i].value = value;
            return;
        }
    }
    if (s->nr_db >= SDXI_MAX_DOORBELLS) {
        error_report("vfio-sdxi: doorbell table full at offset 0x%" PRIx64,
                     offset);
        return;
    }
    s->db[s->nr_db].offset = offset;
    s->db[s->nr_db].value = value;
    s->nr_db++;
}

/* BQL held. Completes the guest store and does not write the BAR. */
static bool vfio_sdxi_trap_mmio_write(VFIODevice *vbasedev, uint8_t region,
                                      uint64_t addr, uint64_t data,
                                      unsigned size)
{
    VFIOSDXIDevice *s = VFIO_SDXI(vbasedev->dev);

    if (!s->swallow_db || region != s->db_region) {
        return false;
    }
    if (size == 8) {
        vfio_sdxi_note_doorbell(s, addr, data);
    }
    return true;
}

/* BQL held. */
static void vfio_sdxi_replay_doorbells(VFIODevice *vbasedev)
{
    VFIOSDXIDevice *s = VFIO_SDXI(vbasedev->dev);
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(vbasedev->dev);
    VFIORegion *region = &vdev->bars[SDXI_DOORBELL_BAR].region;
    uint32_t i;

    if (!s->swallow_db && !s->nr_db) {
        return;
    }
    s->swallow_db = false;
    s->db_region = region->nr;

    for (i = 0; i < s->nr_db; i++) {
        uint64_t le = cpu_to_le64(s->db[i].value);
        int ret;

        ret = vbasedev->io_ops->region_write(vbasedev, region->nr,
                                             s->db[i].offset, 8, &le,
                                             region->post_wr);
        if (ret != 8) {
            error_report("vfio-sdxi: doorbell replay at 0x%" PRIx64
                         " failed: %s",
                         s->db[i].offset, strwriteerror(ret));
        }
    }
    s->nr_db = 0;
    vfio_region_mmaps_set_enabled(region, true);
}

static void vfio_sdxi_instance_init(Object *obj)
{
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(obj);

    vdev->vbasedev.enable_migration = ON_OFF_AUTO_ON;
}

static void vfio_sdxi_realize(DeviceState *dev, Error **errp)
{
    PCIDevice *pdev = PCI_DEVICE(dev);
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(dev);
    VFIOSDXIDevice *s = VFIO_SDXI(dev);
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

    vdev->vbasedev.trap_mmio_write = vfio_sdxi_trap_mmio_write;
    vdev->vbasedev.replay_doorbells = vfio_sdxi_replay_doorbells;
    vmstate_register(VMSTATE_IF(dev), VMSTATE_INSTANCE_ID_ANY,
                     &vmstate_vfio_sdxi, s);
}

static void vfio_sdxi_unrealize(DeviceState *dev)
{
    vmstate_unregister(VMSTATE_IF(dev), &vmstate_vfio_sdxi, VFIO_SDXI(dev));
    if (vfio_sdxi_parent_unrealize) {
        vfio_sdxi_parent_unrealize(dev);
    }
}

static void vfio_sdxi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "VFIO-based SDXI PCI device assignment";
    dc->hotpluggable = false;

    device_class_set_parent_realize(dc, vfio_sdxi_realize,
                                    &vfio_sdxi_parent_realize);
    device_class_set_parent_unrealize(dc, vfio_sdxi_unrealize,
                                      &vfio_sdxi_parent_unrealize);
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
