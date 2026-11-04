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
 * be answered. Doorbell writes are stored from the moment the trap is
 * installed, which is before the function is asked to stop. A write
 * that reaches the device after it has accepted the stop is dropped,
 * and soft-stopping often never appears in MMIO_STS0. The stored
 * doorbells migrate with this device and are written back once it is
 * running again. The device is assumed not to fetch a new descriptor
 * from the write index alone.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/cpus.h"
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
#define SDXI_MMIO_CTL0 0x00000
#define SDXI_MMIO_STS0 0x100
#define SDXI_MMIO_CXT_L2 0x10000
#define SDXI_FN_GSV_MASK 0x7
#define SDXI_GSV_STOP 0
#define SDXI_GSV_ACTIVE 2
#define SDXI_CTL0_FN_PASID_VL (1ULL << 2)
/* MMIO_CXT_L2 pointer is bits 63:12. */
#define SDXI_CXT_L2_PTR (~UINT64_C(0xfff))

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
    trace_vfio_sdxi_pause(vbasedev->name,
                          op == VFIO_DEVICE_FEATURE_SDXI_PAUSE_RESUME ?
                          "resume" : "drain");

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
 * BQL held. During the pause drain, keep a naturally aligned 64-bit
 * doorbell for replay and consume that write. The function ignores any
 * other doorbell access, so a write this trap did not record continues
 * on the normal path.
 */
static bool vfio_sdxi_trap_mmio_write(VFIODevice *vbasedev, uint8_t region,
                                      uint64_t addr, uint64_t data,
                                      unsigned size)
{
    VFIOSDXIDevice *s = VFIO_SDXI(vbasedev->dev);

    if (region != s->db_region || !s->swallow_db || size != 8 || (addr & 7)) {
        return false;
    }
    vfio_sdxi_note_doorbell(s, addr, data);
    trace_vfio_sdxi_note_doorbell(addr, data);
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
    s->swallow_db = false;
    s->nr_db = 0;
    vfio_region_mmaps_set_enabled(region, true);
}

static bool vfio_sdxi_read64(VFIODevice *vbasedev, VFIORegion *bar0,
                             hwaddr offset, uint64_t *val)
{
    uint64_t raw;

    if (bar0->size < offset + sizeof(raw)) {
        return false;
    }
    if (pread(vbasedev->fd, &raw, sizeof(raw),
              bar0->fd_offset + offset) != (ssize_t)sizeof(raw)) {
        return false;
    }
    *val = le64_to_cpu(raw);
    return true;
}

/*
 * Guest SDXI driver never programmed this function. The host has already
 * run sdxi_fn_start(), so fn_gsv is GSV_ACTIVE rather than GSV_STOP and
 * fn_gsr is not GSRV_RESET. No context-table pointer and no function
 * PASID is the state a soft stop cannot quiesce: GSRV_STOP_SF lands in
 * GSV_ERROR. A failed read, or GSV_ERROR, is not this case.
 */
static bool vfio_sdxi_guest_unprogrammed(VFIODevice *vbasedev)
{
    VFIORegion *bar0 = &VFIO_PCI_DEVICE(vbasedev->dev)->bars[0].region;
    uint64_t ctl0, sts0, cxt_l2;
    unsigned gsv;

    if (!vfio_sdxi_read64(vbasedev, bar0, SDXI_MMIO_CTL0, &ctl0) ||
        !vfio_sdxi_read64(vbasedev, bar0, SDXI_MMIO_STS0, &sts0) ||
        !vfio_sdxi_read64(vbasedev, bar0, SDXI_MMIO_CXT_L2, &cxt_l2)) {
        return false;
    }
    if (ctl0 == UINT64_MAX) {
        return false;
    }
    gsv = sts0 & SDXI_FN_GSV_MASK;
    if (gsv != SDXI_GSV_STOP && gsv != SDXI_GSV_ACTIVE) {
        return false;
    }
    return !(cxt_l2 & SDXI_CXT_L2_PTR) && !(ctl0 & SDXI_CTL0_FN_PASID_VL);
}

static int vfio_sdxi_pause_prepare(VFIODevice *vbasedev)
{
    VFIOSDXIDevice *s = VFIO_SDXI(vbasedev->dev);
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(vbasedev->dev);
    VFIORegion *region = &vdev->bars[SDXI_DOORBELL_BAR].region;
    VFIORegion *bar0 = &vdev->bars[0].region;
    int ret;

    trace_vfio_sdxi_pause_prepare(vbasedev->name);
    vbasedev->pause_failed = false;
    if (!region->size) {
        error_report("vfio-sdxi: BAR2 is not mapped; cannot hold doorbells");
        return -ENODEV;
    }

    /*
     * Store doorbells before asking the function to stop. region_write
     * holds the BQL across the device write, so taking it here means no
     * forwarded doorbell is still in flight. The drain ioctl runs after
     * the unlock.
     */
    bql_lock();
    s->db_region = region->nr;
    s->nr_db = 0;
    s->swallow_db = true;
    vfio_region_mmaps_set_enabled(region, false);
    bql_unlock();

    /*
     * Do not ask the function to soft-stop when the guest never
     * programmed it. The doorbell trap stays installed; there is no
     * context to ring.
     */
    if (vfio_sdxi_guest_unprogrammed(vbasedev)) {
        info_report("vfio-sdxi: %s: guest SDXI driver not programmed; "
                    "skip pause drain", vbasedev->name);
        return 0;
    }

    ret = vfio_sdxi_pause_ioctl(vbasedev, VFIO_DEVICE_FEATURE_SDXI_PAUSE_DRAIN);
    /*
     * Debug hold. The function is still in the state the driver sampled
     * (GSV_ERROR, or whatever gsv accompanied a nonempty error log). No
     * further MMIO and no device reset. Freeze the vCPUs here, without
     * vm_stop(), so the VFIO notifiers do not run STOP_COPY or sdxi_start.
     * Never return: migration_stop_vm() would otherwise fail the migration
     * and vm_start() would run the guest again.
     */
    if (ret == -EPROTO) {
        QemuSemaphore hold;
        uint64_t sts = UINT64_MAX;
        unsigned gsv = 0xff;

        if (bar0->size >= SDXI_MMIO_STS0 + sizeof(sts)) {
            ssize_t n = pread(vbasedev->fd, &sts, sizeof(sts),
                              bar0->fd_offset + SDXI_MMIO_STS0);

            if (n == (ssize_t)sizeof(sts)) {
                gsv = le64_to_cpu(sts) & SDXI_FN_GSV_MASK;
            }
        }
        error_report("vfio-sdxi: %s: holding VM with vCPUs paused; "
                     "fn_gsv=%u preserved, no reset",
                     vbasedev->name, gsv);
        vbasedev->pause_failed = true;
        bql_lock();
        pause_all_vcpus();
        bql_unlock();
        qemu_sem_init(&hold, 0);
        for (;;) {
            qemu_sem_wait(&hold);
        }
    }
    if (!ret) {
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

    trace_vfio_sdxi_pause_rollback(vbasedev->name);
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
