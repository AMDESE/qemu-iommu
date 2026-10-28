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
 * Before vCPUs are paused, BAR2 is trapped and the function is drained
 * with VFIO_DEVICE_FEATURE_SDXI_PAUSE so an outstanding PRI can still
 * be answered. Doorbell writes during the drain are delivered to the
 * device. After the function stops, further doorbell writes are held
 * here, migrated with this device, and written back once it is running
 * again. The device is assumed not to fetch a new descriptor from the
 * write index alone.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "pci.h"
#include "vfio-region.h"
#include "qom/object.h"
#include "trace.h"
#include <linux/vfio.h>
#include <sys/ioctl.h>

#define SDXI_DOORBELL_BAR 2
#define SDXI_MAX_DOORBELLS 128
#define SDXI_PAUSE_DRAIN_MS 30000

#define TYPE_VFIO_SDXI "vfio-sdxi"
OBJECT_DECLARE_SIMPLE_TYPE(VFIOSDXIDevice, VFIO_SDXI)

typedef struct SdxiDoorbell {
    uint64_t offset;
    uint64_t value;
} SdxiDoorbell;

struct VFIOSDXIDevice {
    VFIOPCIDevice parent_obj;

    bool swallow_db;
    bool forward_db;
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

static int vfio_sdxi_pause_ioctl(VFIODevice *vbasedev, uint32_t op)
{
    uint64_t buf[DIV_ROUND_UP(sizeof(struct vfio_device_feature) +
                              sizeof(struct vfio_device_feature_sdxi_pause),
                              sizeof(uint64_t))] = {};
    struct vfio_device_feature *feature = (struct vfio_device_feature *)buf;
    struct vfio_device_feature_sdxi_pause *pause =
        (struct vfio_device_feature_sdxi_pause *)feature->data;

    feature->argsz = sizeof(buf);
    feature->flags = VFIO_DEVICE_FEATURE_SET | VFIO_DEVICE_FEATURE_SDXI_PAUSE;
    pause->flags = op;
    pause->timeout_ms = SDXI_PAUSE_DRAIN_MS;

    if (ioctl(vbasedev->fd, VFIO_DEVICE_FEATURE, feature)) {
        return -errno;
    }
    return 0;
}

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

/*
 * BQL held. While the drain is in progress, return false so the write
 * reaches the BAR. After GSV_STOP, swallow the write and keep it for
 * replay. Any other write is left for the normal path.
 */
static bool vfio_sdxi_trap_mmio_write(VFIODevice *vbasedev, uint8_t region,
                                      uint64_t addr, uint64_t data,
                                      unsigned size)
{
    VFIOSDXIDevice *s = VFIO_SDXI(vbasedev->dev);

    if (region != s->db_region || (!s->swallow_db && !s->forward_db)) {
        return false;
    }
    if (s->forward_db) {
        /*
         * The write is delivered below. Leave it out of the replay
         * table so a later resume does not ring the same doorbell twice.
         */
        if (size == 8) {
            trace_vfio_sdxi_forward_doorbell(addr, data);
        }
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

/* BQL held. Hand BAR2 back without writing a doorbell a second time. */
static void vfio_sdxi_release_db_trap(VFIOSDXIDevice *s, VFIORegion *region)
{
    s->forward_db = false;
    s->swallow_db = false;
    s->nr_db = 0;
    vfio_region_mmaps_set_enabled(region, true);
}

static int vfio_sdxi_pause_prepare(VFIODevice *vbasedev)
{
    VFIOSDXIDevice *s = VFIO_SDXI(vbasedev->dev);
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(vbasedev->dev);
    VFIORegion *region = &vdev->bars[SDXI_DOORBELL_BAR].region;
    int ret;

    vbasedev->pause_failed = false;
    if (!region->size) {
        error_report("vfio-sdxi: BAR2 is not mapped; cannot hold doorbells");
        return -ENODEV;
    }

    /*
     * Trap BAR2 but still write each doorbell through. The guest can
     * retire the descriptor the soft-stop is waiting on. Swallow only
     * once the function has reached GSV_STOP.
     */
    bql_lock();
    s->db_region = region->nr;
    s->nr_db = 0;
    s->swallow_db = false;
    s->forward_db = true;
    vfio_region_mmaps_set_enabled(region, false);
    bql_unlock();

    ret = vfio_sdxi_pause_ioctl(vbasedev, VFIO_DEVICE_FEATURE_SDXI_PAUSE_DRAIN);
    if (!ret) {
        bql_lock();
        s->forward_db = false;
        s->swallow_db = true;
        s->nr_db = 0;
        bql_unlock();
        return 0;
    }

    error_report("vfio-sdxi: pause drain failed: %s", strerror(-ret));
    bql_lock();
    vfio_sdxi_release_db_trap(s, region);
    vbasedev->pause_failed = true;
    bql_unlock();
    return ret;
}

static void vfio_sdxi_pause_rollback(VFIODevice *vbasedev)
{
    int ret;

    ret = vfio_sdxi_pause_ioctl(vbasedev, VFIO_DEVICE_FEATURE_SDXI_PAUSE_RESUME);
    if (ret) {
        error_report("vfio-sdxi: pause resume failed: %s", strerror(-ret));
    }
    bql_lock();
    vfio_sdxi_replay_doorbells(vbasedev);
    bql_unlock();
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

    vdev->vbasedev.pause_prepare = vfio_sdxi_pause_prepare;
    vdev->vbasedev.pause_rollback = vfio_sdxi_pause_rollback;
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
