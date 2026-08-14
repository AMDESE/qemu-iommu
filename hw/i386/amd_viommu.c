#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/blocker.h"
#include "migration/misc.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/i386/pc.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "trace.h"

#include "amd_iommu.h"
#include "amd_viommu.h"

#include "system/runstate.h"
#include "hw/core/iommu.h"
#include "hw/vfio/vfio-iommufd.h"
#include "system/iommufd.h"
#include "system/kvm.h"

/*
 * Mapping 3st 4K (VF MMIO space)
 * Must be called after out_vfmmio_mmap_offset is set
 * when amdvi_viommu_initialized_one() is called.
 */
static void amd_viommu_vm_state_change(void *opaque,
                                        bool running, RunState state);
static void amd_viommu_vm_state_change_prepare(void *opaque,
                                               bool running, RunState state);

/*
 * vm change state handler priority for the vIOMMU.
 *
 * Handlers are kept sorted by ascending priority and are run forwards when the
 * VM starts and backwards when it stops.  A single relationship therefore
 * covers both directions: the vIOMMU must sort strictly before any VFIO
 * endpoint behind it, so that on resume the vIOMMU is set up before the
 * endpoint starts issuing DMA, and on stop the endpoint is stopped before the
 * vIOMMU state it depends on is touched.
 *
 * VFIO registers via qdev_add_vm_change_state_handler_full(), which uses the
 * device's qdev tree depth as the priority.  Any device sitting on a bus has a
 * depth of at least one, so zero keeps the vIOMMU ahead of every endpoint.
 * The depth helper is private to hw/core, so the relationship cannot be
 * checked against VFIO at run time; pin our side of it here instead.
 *
 * Priority alone is not enough on resume.  vm_state_notify() runs every
 * prepare callback before it runs any ordinary one, and VFIO uses a prepare
 * callback to move the endpoint to RUNNING_P2P, which is a DMA-capable state.
 * An endpoint with work still queued from the source starts that DMA there, so
 * anything the vIOMMU has to put back has to be back before the prepare phase
 * reaches VFIO.  Resume therefore runs as a prepare callback of our own, where
 * the priority above orders us ahead of VFIO's.
 */
#define AMD_VIOMMU_VM_STATE_PRIO 0
QEMU_BUILD_BUG_ON(AMD_VIOMMU_VM_STATE_PRIO >= 1);

/*
 * Budget PREPARE_SAVE gets to drain the guest command ring and fence the
 * completions behind it.  The kernel uses the value as given and has no
 * default of its own, so zero would ask it to work to a deadline that has
 * already passed.
 */
#define AMD_VIOMMU_MIG_DRAIN_TIMEOUT_MS 5000

static void amd_viommu_state_change_shutdown(AMDVIState *s);

static void amdvi_handle_cmdbase_write(AMDVIState *s, unsigned long ctrl,
                                       bool masked);
static void amdvi_handle_evtbase_write(AMDVIState *s, unsigned long ctrl,
                                       bool masked);
static void amdvi_handle_pprbase_write(AMDVIState *s, unsigned long ctrl,
                                       bool masked);

static void amdvi_free_cmdbuf_hwq(AMDVIState *s);
static void amdvi_free_evtlog_hwq(AMDVIState *s);
static void amdvi_free_pprlog_hwq(AMDVIState *s);

static uint64_t amdvi_readq(AMDVIState *s, hwaddr addr);

static int amd_viommu_mmap_vf_mmio(AMDVIState *s)
{
    char *name;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint64_t baseaddr = AMDVI_BASE_ADDR + (x86_iommu->index * AMDVI_MMIO_SIZE);

    /*
     * There is one VF MMIO page per vIOMMU, but this is reached once per
     * attached VFIO device from amd_viommu_state_change_running().  Mapping it
     * again would leak the previous mapping and re-register &s->mr_vf_mmio,
     * which is already a subregion of system memory.
     */
    if (s->vf_mmio_page) {
        return 0;
    }

    s->vf_mmio_page = mmap(NULL,
                         AMDVI_PAGE_SIZE,
                         PROT_READ | PROT_WRITE,
			             MAP_SHARED,
                         s->iommufd->fd,
                         s->iommufd_viommu_amd.out_vfmmio_mmap_offset);

    if (s->vf_mmio_page == MAP_FAILED) {
	    error_report("Failed to mmap VF MMIO");
	    s->vf_mmio_page = NULL;
	    return -EIO;
    }

    name = g_strdup_printf("vf-mmio");
    memory_region_init_ram_device_ptr(&s->mr_vf_mmio,
                                      memory_region_owner(&s->mr_vf_mmio),
                                      name,
                                      0x1000,
                                      s->vf_mmio_page);
    /*
     * This is a vf-mmio region that is not used for DMA.
     * It is needs to be mapped as RAM in order to allow CPU nested pagetable
     * to map for the vf-mmio region.
     */
    memory_region_set_skip_vfio_dma(&s->mr_vf_mmio, true);

    memory_region_add_subregion_overlap(get_system_memory(),
                                        baseaddr + 0x2000,
                                        &s->mr_vf_mmio,
                                        1);
    g_free(name);
    return 0;
}

struct AMDVI_dte_key {
    PCIBus *bus;
    uint8_t devfn;
};

static void amdvi_alloc_passthrough_hwpt(AMDIOMMUFDDevice *dev, AMDVIState *s)
{

    struct iommu_hwpt_amd_guest hwpt = {{0,0,0,0}};
    VFIODevice *vdev = dev->hiod->agent;
    Error *local_err = NULL;
    uint32_t hwpt_id;
    int ret;


    ret = iommufd_backend_alloc_hwpt(s->iommufd,
                                     vdev->idev.dev_id,
                                     s->core->viommu_id,
                                     0,
                                     IOMMU_HWPT_DATA_AMD_GUEST,
                                     sizeof(hwpt), &hwpt, &hwpt_id, &local_err);

    assert(ret == true);

    dev->passthrough_hwpt_id = hwpt_id;
    dev->v2_hwpt_id = 0;
}

static int amdvi_viommu_initialized_one(AMDVIState *s, AMDIOMMUFDDevice *amd_idev)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

    s->core = iommufd_backend_alloc_viommu(s->iommufd,
                                           idev->devid,
                                           IOMMU_VIOMMU_TYPE_AMD,
                                           amd_idev->v1_hwpt.hwpt_id,
                                           sizeof(s->iommufd_viommu_amd),
                                           &s->iommufd_viommu_amd);
    if (!s->core) {
        error_report("failed to allocate a viommu");
        return -EINVAL;
    }

    /*
     * Every other device has to be on this same nesting parent, since the
     * kernel object just allocated describes that one.
     */
    s->nest_parent_hwpt_id = amd_idev->v1_hwpt.hwpt_id;

    /*
     * The capability describes the host IOMMU this vIOMMU was allocated on. It
     * says that IOMMU can migrate a vIOMMU, not that it can migrate this one:
     * a vIOMMU of a type it cannot handle still refuses IOMMU_VIOMMU_MIG_STATE.
     */
    s->mig_supported = !!(amd_idev->hiod->caps.hw_caps &
                          IOMMU_HW_CAP_VIOMMU_MIGRATION);
    trace_amdvi_viommu_mig_supported(idev->devid, s->mig_supported);

    s->enabled = true;
    return 0;
}

/*
 * What has to hold before this vIOMMU can be saved and restored.  Evaluated
 * over the endpoints assigned right now rather than at realize time, because
 * these are properties of the whole set of devices and that set is not known
 * until the guest turns the vIOMMU on.
 *
 * Reports the first one that fails.  Someone reading the message has to change
 * one thing at a time regardless, and listing every reason at once describes a
 * configuration that nobody asked for.
 */
static bool amdvi_mig_preconditions_met(AMDVIState *s, Error **errp)
{
    struct AMDVI_dte_key *key;
    AMDIOMMUFDDevice *amd_idev;
    GHashTableIter it;

    if (!s->mig_supported) {
        error_setg(errp, "amd-viommu: the host IOMMU cannot save and restore "
                   "vIOMMU state");
        return false;
    }

    g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

    while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
        VFIODevice *vdev = amd_idev->hiod->agent;

        if (amd_idev->v1_hwpt.hwpt_id != s->nest_parent_hwpt_id) {
            error_setg(errp, "amd-viommu: device %s is not on the nesting "
                       "parent this vIOMMU was allocated on", vdev->name);
            return false;
        }

        /*
         * The IOMMU's own writes to guest memory land in pages no device
         * tracker reports, and behind a nesting vIOMMU the device tracker is
         * refused outright, so this is the only mechanism that sees them.
         */
        if (!vdev->hwpt ||
            !(vdev->hwpt->hwpt_flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING)) {
            error_setg(errp, "amd-viommu: the nesting parent of device %s was "
                       "allocated without dirty tracking", vdev->name);
            return false;
        }
    }

    return true;
}

/*
 * Refuse migration up front, with a reason, rather than letting it fail partway
 * through with the guest already stopped.  Called once every assigned device
 * has been set up, because a precondition about the set of devices cannot be
 * evaluated from inside the loop that builds it.
 */
static void amdvi_mig_blocker_update(AMDVIState *s)
{
    Error *local_err = NULL;
    Error *reason = NULL;

    /* Frees the previous reason, so this is also how a blocker is lifted. */
    migrate_del_blocker(&s->mig_blocker);

    /*
     * Nothing has been allocated that would need saving.  A guest that never
     * turns its vIOMMU on stays migratable.
     */
    if (!s->core) {
        return;
    }

    if (amdvi_mig_preconditions_met(s, &reason)) {
        return;
    }

    s->mig_blocker = reason;
    if (migrate_add_blocker(&s->mig_blocker, &local_err) < 0) {
        /*
         * Either a migration is already under way, which is past the point
         * where refusing it here would help, or --only-migratable was given
         * and the guest is running anyway.  The save path checks the same
         * preconditions and can still fail the stream.
         */
        error_report_err(local_err);
    }
}

static int amd_viommu_setup_nest_parent(AMDIOMMUFDDevice *dev)
{
    VFIODevice *vdev = dev->hiod->agent;
    VFIOIOASHwpt *hwpt = vdev->hwpt;
    VFIOIOMMUFDContainer *ioc = VFIO_IOMMU_IOMMUFD(vdev->bcontainer);

    if (!hwpt || !(hwpt->hwpt_flags & IOMMU_HWPT_ALLOC_NEST_PARENT)) {
        error_report("%s: device %s missing VFIO nesting parent HWPT "
                     "(VIOMMU_FLAG_WANT_NESTING_PARENT)",
                     __func__, vdev->name);
        return -EINVAL;
    }

    dev->v1_hwpt.hwpt_id = hwpt->hwpt_id;
    dev->v1_hwpt.parent_ioas_id = ioc->ioas_id;

    return 0;
}

/*
 * Note: This must be called when running state is RUN_STATE_RUNNING
 */
static void amdvi_vdevice_viommu_setup(AMDVIState *s, AMDIOMMUFDDevice *amd_idev)
{
    if (amd_viommu_setup_nest_parent(amd_idev)) {
        return;
    }

    /*
     * The iommufd vIOMMU object and its VF MMIO page are per-vIOMMU and are
     * shared by every attached device, whereas the passthrough domain is
     * per-device.  This runs once per device, and again for every device when
     * restoring, so each step carries its own guard.
     */
    if (!s->enabled && amdvi_viommu_initialized_one(s, amd_idev)) {
        return;
    }

    if (amd_viommu_mmap_vf_mmio(s)) {
        return;
    }

    if (!amd_idev->passthrough_hwpt_id) {
        amdvi_alloc_passthrough_hwpt(amd_idev, s);
    }
}

/*
 * Allocate the iommufd vDEVICE for every device behind this vIOMMU.  A vDEVICE
 * binds one host device to the guest RID the guest addresses it by, so the two
 * have to come from the same device: the hash key supplies both.
 *
 * This depends on guest-assigned PCI bus numbers, because the virtual RID comes
 * from pci_bus_num(), which for any non-root bus reads PCI_SECONDARY_BUS out of
 * the parent bridge's config space.  That byte is zero until the guest
 * enumerates the bus, so this must not run before guest enumeration or every
 * device behind a bridge is given a RID on bus 0.
 *
 * On cold boot the guest's first MMIO access to the IOMMU serves as the
 * "enumeration finished" signal.  A restored guest has already enumerated and
 * may never touch those registers again, so the restore path calls this
 * directly once PCI config space has been loaded.  Hence the guard: it must be
 * safe to reach from either.
 */
static void vdev_alloc_all(AMDVIState *s)
{
    struct AMDVI_dte_key *key;
    AMDIOMMUFDDevice *amd_idev;
    GHashTableIter it;

    if (s->vdevice_setup) {
        return;
    }

    g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

    /* Go through each VFIO device */
    while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
        HostIOMMUDeviceIOMMUFD *idev =
            HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
        uint16_t rid = PCI_BUILD_BDF(pci_bus_num(key->bus), key->devfn);

        if (amd_idev->core) {
            continue;
        }

        amd_idev->core = iommufd_backend_alloc_vdev(idev, s->core, rid);
        if (!amd_idev->core) {
            error_report("failed to allocate a vDEVICE for %02x:%02x.%x",
                         pci_bus_num(key->bus), PCI_SLOT(key->devfn),
                         PCI_FUNC(key->devfn));
        }
    }

    s->vdevice_setup = true;
}

static void amdvi_mig_restore(AMDVIState *s);
static void amdvi_mig_payload_reset(AMDVIState *s);
static void amdvi_mig_restore_failed(AMDVIState *s, Error *err);
static bool amdvi_devtab_restore(AMDVIState *s, Error **errp);
static AMDIOMMUFDDevice *amd_viommu_get_device_from_bdf(AMDVIState *s,
                                                        uint16_t bus,
                                                        uint16_t devfn);
static int amd_viommu_update_gcr3(AMDVIState *s, AMDIOMMUFDDevice *dev,
                                  uint16_t dev_id, uint64_t *dte);
static inline int amdvi_handle_xt_event_int_write(AMDVIState *s);
static inline int amdvi_handle_xt_ppr_int_write(AMDVIState *s);

static void amd_viommu_state_change_running(AMDVIState *s)
{
    struct AMDVI_dte_key *key;
    AMDIOMMUFDDevice *amd_idev;
    Error *local_err = NULL;
    GHashTableIter it;

    /*
     * Before the devices, because a restored guest never re-issues the
     * register write that would otherwise build the table, and the DTEs it
     * holds have to be readable the moment the guest runs again.
     */
    if (!amdvi_devtab_restore(s, &local_err)) {
        amdvi_mig_restore_failed(s, local_err);
        return;
    }

    g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

    /* Go through each VFIO device */
    while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
        amdvi_vdevice_viommu_setup(s, amd_idev);
    }

    /* The vIOMMU and its endpoints exist only after that loop. */
    amdvi_mig_blocker_update(s);

    /*
     * The vDEVICEs are normally allocated from the guest's first IOMMU MMIO
     * write, which is what tells us PCI enumeration has finished.  A restored
     * guest enumerated on the source and may never write those registers
     * again, so nothing would allocate them and every later attach would fail
     * to resolve the device.  Its bus numbers arrived with the migrated PCI
     * config space, so the enumeration this waits for has already happened.
     */
    if (s->mig_restore_pending) {
        vdev_alloc_all(s);
    }

    /* The vIOMMU exists only after the loop above, so restore comes last. */
    amdvi_mig_restore(s);

    /*
     * The nested domains are rebuilt from inside that restore, because they
     * have to land before COMMIT_RESTORE.  A stream carrying device records
     * therefore has to carry the vIOMMU payload as well, which the same save
     * always produces; catch it here rather than resume a guest whose devices
     * were quietly left without their domains.
     */
    if (s->dev_replay_pending) {
        error_setg(&local_err, "amd-viommu: migrated device records arrived "
                   "without the vIOMMU state they have to be replayed with");
        amdvi_mig_restore_failed(s, local_err);
    }
}

static void amdvi_free_cmdbuf_hwq(AMDVIState *s)
{
    if (!s->cmdbuf_hwq) {
        return;
    }
    iommufd_backend_free_id(s->iommufd, s->cmdbuf_hwq->hw_queue_id);
    g_free(s->cmdbuf_hwq);
    s->cmdbuf_hwq = NULL;
}

static void amdvi_free_evtlog_hwq(AMDVIState *s)
{
    if (!s->evtlog_hwq) {
        return;
    }
    iommufd_backend_free_id(s->iommufd, s->evtlog_hwq->hw_queue_id);
    g_free(s->evtlog_hwq);
    s->evtlog_hwq = NULL;
}

static void amdvi_free_pprlog_hwq(AMDVIState *s)
{
    if (!s->pprlog_hwq) {
        return;
    }
    iommufd_backend_free_id(s->iommufd, s->pprlog_hwq->hw_queue_id);
    g_free(s->pprlog_hwq);
    s->pprlog_hwq = NULL;
}

static void amd_viommu_state_change_shutdown(AMDVIState *s)
{
    Error *errp = NULL;
    struct AMDVI_dte_key *key;
    GHashTableIter amd_idev_it;
    AMDIOMMUFDDevice *amd_idev;
    HostIOMMUDeviceIOMMUFD *idev;

    if (s->vf_mmio_page) {
        munmap(s->vf_mmio_page, AMDVI_PAGE_SIZE);
        s->vf_mmio_page = NULL;
    }

    /* No-op unless the capability was missing.  Frees the reason. */
    migrate_del_blocker(&s->mig_blocker);
    amdvi_mig_payload_reset(s);

    if (s->mig_fail_bh) {
        qemu_bh_delete(s->mig_fail_bh);
        s->mig_fail_bh = NULL;
    }

    amdvi_free_pprlog_hwq(s);
    amdvi_free_evtlog_hwq(s);
    amdvi_free_cmdbuf_hwq(s);

    if (s->amd_iommufd_dev_hash) {
        g_hash_table_iter_init(&amd_idev_it, s->amd_iommufd_dev_hash);
        while (g_hash_table_iter_next(&amd_idev_it, (void **)&key,
                                      (void **)&amd_idev)) {
            VFIODevice *vdev_detach = amd_idev->hiod->agent;
            VFIOIOMMUFDContainer *ioc_detach =
                VFIO_IOMMU_IOMMUFD(vdev_detach->bcontainer);
            uint32_t ioas_detach = ioc_detach->ioas_id;

            /*
             * The devices were attached to the nesting parent domain, which
             * is going away, so put them back on the container's IOAS.
             */
            idev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);
            if (!host_iommu_device_iommufd_attach_hwpt(idev, ioas_detach,
                                                       &errp)) {
                error_free(errp);
                errp = NULL;
            }

            if (amd_idev->v2_hwpt_id)
                iommufd_backend_free_id(s->iommufd, amd_idev->v2_hwpt_id);
            if (amd_idev->core && amd_idev->core->vdev_id)
                iommufd_backend_free_id(s->iommufd, amd_idev->core->vdev_id);
            if (amd_idev->passthrough_hwpt_id)
                iommufd_backend_free_id(s->iommufd, amd_idev->passthrough_hwpt_id);

            /*
             * Only what was allocated from the vIOMMU goes away.  The entry
             * belongs to the hash, which frees it when the device is detached,
             * so freeing it here would leave a dangling pointer behind and
             * free it a second time later.
             */
            g_free(amd_idev->core);
            amd_idev->core = NULL;
            amd_idev->v2_hwpt_id = 0;
            amd_idev->passthrough_hwpt_id = 0;
        }
    }

    /* No device holds a vDEVICE any more, so the flag saying they do is stale */
    s->vdevice_setup = false;

    if (s->core && s->core->viommu_id)
        iommufd_backend_free_id(s->iommufd, s->core->viommu_id);
    g_free(s->core);
    s->core = NULL;
}
static void amdvi_mig_payload_reset(AMDVIState *s)
{
    g_free(s->mig_blob);
    s->mig_blob = NULL;
    s->mig_blob_len = 0;
    s->mig_hdr_version = 0;
    s->mig_hdr_payload_size = 0;
    memset(&s->mig_hdr, 0, sizeof(s->mig_hdr));
}

/*
 * Hand a quiesced vIOMMU back to the guest.  Only reachable when the save
 * failed, and there is nothing to do about a failure to unwind beyond saying
 * so, since the caller is already on an error path.
 */
static void amdvi_mig_abort(AMDVIState *s)
{
    Error *local_err = NULL;

    if (!iommufd_backend_viommu_mig_state(s->iommufd, s->core->viommu_id,
                                          IOMMU_VIOMMU_MIG_OP_ABORT, 0, NULL,
                                          NULL, NULL, NULL, &local_err)) {
        warn_report_err(local_err);
    }
}

/*
 * Park the kernel's migration payload where VMState can pick it up.
 *
 * A failure has to abort the migration rather than just complain: leaving the
 * payload empty stops the subsection from being sent, and the destination would
 * then resume on an unconfigured vIOMMU with nothing to say why.  Run state
 * handlers cannot report failures, so take the same route as VFIO and set the
 * error on the migration stream.  It takes ownership of the Error, and the code
 * only has to be non-zero because the Error carries the detail.
 */
static void amdvi_mig_save(AMDVIState *s)
{
    struct iommu_viommu_mig_header hdr = {};
    uint32_t len = 0, status = 0;
    Error *local_err = NULL;
    uint8_t *blob;

    if (!s->core) {
        return;
    }

    amdvi_mig_payload_reset(s);

    /*
     * The same preconditions the blocker was built from.  Checked again here
     * because the blocker can only be registered while no migration is in
     * flight, so it is an early refusal rather than the guarantee.
     */
    if (!amdvi_mig_preconditions_met(s, &local_err)) {
        migration_file_set_error(-ENOTSUP, local_err);
        return;
    }

    /*
     * Quiesce before sizing.  PREPARE_SAVE drains the guest command ring and
     * freezes log generation, so from here the payload stops growing and the
     * size asked for below is still the size SAVE needs.  Sizing first would
     * race a guest that is still filling the rings.
     */
    if (!iommufd_backend_viommu_mig_state(s->iommufd, s->core->viommu_id,
                                          IOMMU_VIOMMU_MIG_OP_PREPARE_SAVE,
                                          AMD_VIOMMU_MIG_DRAIN_TIMEOUT_MS,
                                          NULL, NULL, NULL, &status,
                                          &local_err)) {
        migration_file_set_error(-EIO, local_err);
        return;
    }
    trace_amdvi_mig_prepared(s->core->viommu_id, status);

    if (!iommufd_backend_viommu_mig_state(s->iommufd, s->core->viommu_id,
                                          IOMMU_VIOMMU_MIG_OP_GET_SIZE, 0,
                                          NULL, NULL, &len, NULL, &local_err)) {
        migration_file_set_error(-EIO, local_err);
        goto abort;
    }

    /*
     * SAVE reports the size it needs instead of consuming the quiesce when the
     * buffer is short, but arriving there would mean the payload grew after
     * being frozen, so treat it as the error it is rather than retrying.
     */
    blob = g_malloc0(len);
    if (!iommufd_backend_viommu_mig_state(s->iommufd, s->core->viommu_id,
                                          IOMMU_VIOMMU_MIG_OP_SAVE, 0, &hdr,
                                          blob, &len, NULL, &local_err)) {
        migration_file_set_error(-EIO, local_err);
        g_free(blob);
        goto abort;
    }

    s->mig_hdr = hdr;
    s->mig_blob = blob;
    s->mig_blob_len = len;
    s->mig_hdr_version = hdr.blob_format_version;
    s->mig_hdr_payload_size = hdr.payload_size;
    trace_amdvi_mig_saved(s->core->viommu_id, len, hdr.blob_format_version);
    return;

abort:
    amdvi_mig_abort(s);
}

static void amdvi_mig_restore_failed_bh(void *opaque)
{
    AMDVIState *s = opaque;

    qemu_bh_delete(s->mig_fail_bh);
    s->mig_fail_bh = NULL;
    vm_stop(RUN_STATE_INTERNAL_ERROR);
}

/*
 * Stop the guest rather than let it run against a vIOMMU that was never
 * restored.  Failing the migration is not available here: this runs from the
 * RUN_STATE_RUNNING notifier, which process_incoming_migration_bh() reaches
 * only after it is committed to running the guest on this host, and the
 * incoming migration is marked complete immediately afterwards.
 *
 * The stop is deferred to a bottom half because vm_start() resumes the vCPUs
 * unconditionally once the notifier chain returns, so stopping from inside it
 * would simply be undone on the way out.
 */
static void amdvi_mig_restore_failed(AMDVIState *s, Error *err)
{
    error_reportf_err(err, "amd-viommu: stopping the guest: ");

    if (!s->mig_fail_bh) {
        s->mig_fail_bh = qemu_bh_new(amdvi_mig_restore_failed_bh, s);
    }
    qemu_bh_schedule(s->mig_fail_bh);
}

/*
 * Apply a payload that arrived in VMState, once the vIOMMU it belongs to has
 * been allocated.  Every kernel operation unwinds itself on failure, so there
 * is no abort to issue here.
 */
/*
 * Recreate the hardware queues the source had, reading their configuration out
 * of the already-restored register file.
 *
 * MIG_STATE RESTORE writes head, tail, status and ring contents into queues
 * that must already exist, so this has to run before it.  The queues are
 * allocated with their enable bits masked and are armed by COMMIT_RESTORE
 * (D-015), which keeps the window between the two from letting the hardware
 * consume a ring the kernel has not finished filling in.
 *
 * The rings selected here are the ones amdvi_handle_control_write() would have
 * allocated on the source for the same control value, so a ring the guest
 * never enabled stays unallocated on the destination too.
 */
static int amdvi_mig_replay_hw_queues(AMDVIState *s, Error **errp)
{
    unsigned long ctrl = amdvi_readq(s, AMDVI_MMIO_CONTROL);

    if (ctrl & (1 << 12)) {
        amdvi_handle_cmdbase_write(s, ctrl, true);
        if (!s->cmdbuf_hwq) {
            error_setg(errp, "amd-viommu: failed to replay the command buffer");
            return -EIO;
        }
    }
    if (ctrl & (1 << 2)) {
        amdvi_handle_evtbase_write(s, ctrl, true);
        if (!s->evtlog_hwq) {
            error_setg(errp, "amd-viommu: failed to replay the event log");
            return -EIO;
        }
    }
    if (ctrl & (1 << 13)) {
        amdvi_handle_pprbase_write(s, ctrl, true);
        if (!s->pprlog_hwq) {
            error_setg(errp, "amd-viommu: failed to replay the PPR log");
            return -EIO;
        }
    }

    trace_amdvi_mig_replayed_hw_queues(s->core->viommu_id, ctrl);

    return 0;
}

/*
 * Rebuild the nested domains the source had, one per assigned device.
 *
 * This calls amd_viommu_update_gcr3() rather than driving the DTE write
 * handler, whose allocation only fires when the guest writes DTE[0] and DTE[1]
 * adjacently.  A restored guest issues no such write, and the handler would
 * also have to be fed a write sequence that never happened.
 *
 * dte_info is then seeded with what each domain was built from, so that the
 * guest's next write to a DTE it has not changed is recognised as a no-op
 * instead of allocating the domain a second time.
 */
static bool amdvi_dev_replay_nested(AMDVIState *s, Error **errp)
{
    g_autofree AMDVI_dev_replay *records = NULL;
    int32_t count = s->dev_replay_count;
    int32_t i;

    if (!s->dev_replay_pending) {
        return true;
    }
    s->dev_replay_pending = false;

    records = g_steal_pointer(&s->dev_replay);
    s->dev_replay_count = 0;

    for (i = 0; i < count; i++) {
        AMDVI_dev_replay *rec = &records[i];
        AMDVI_dte_info *info = &s->dte_info[rec->devid];
        AMDIOMMUFDDevice *dev;

        if (rec->had_nested) {
            dev = amd_viommu_get_device_from_bdf(s, rec->devid >> 8,
                                                 rec->devid & 0xFF);
            if (!dev) {
                error_setg(errp, "amd-viommu: device %04x lost between load "
                           "and restore", rec->devid);
                return false;
            }

            if (amd_viommu_update_gcr3(s, dev, rec->devid, rec->dte)) {
                error_setg(errp, "amd-viommu: failed to rebuild the nested "
                           "domain for device %04x", rec->devid);
                return false;
            }
        }

        info->dte0 = rec->dte[0];
        info->dte1 = rec->dte[1];
        info->last = rec->last;
        info->curr = rec->curr;
    }

    trace_amdvi_mig_replayed_nested(count);

    return true;
}

/*
 * Re-establish the EventLog and PPRLog interrupt routes.
 *
 * These are host state: an ext-IRTE names a destination vCPU and vector on the
 * host the vIOMMU lives on, so nothing the source captured can be handed over
 * and the destination has to program its own from the guest's XT registers.
 * Those registers cross in mmior[], and a guest writing one is exactly what
 * drives this on a cold boot, so the same handlers are reused here.
 *
 * A register the guest never programmed reads back zero, which the remap call
 * treats as nothing to do.
 */
static bool amdvi_mig_replay_ext_int(AMDVIState *s, Error **errp)
{
    if (amdvi_handle_xt_event_int_write(s)) {
        error_setg(errp, "amd-viommu: failed to replay the event log "
                   "interrupt route");
        return false;
    }

    if (amdvi_handle_xt_ppr_int_write(s)) {
        error_setg(errp, "amd-viommu: failed to replay the PPR log "
                   "interrupt route");
        return false;
    }

    trace_amdvi_mig_replayed_ext_int(amdvi_readq(s, AMDVI_MMIO_XT_EVENT_INT),
                                     amdvi_readq(s, AMDVI_MMIO_XT_PPR_INT));

    return true;
}

static void amdvi_mig_restore(AMDVIState *s)
{
    Error *local_err = NULL;
    uint32_t len;

    if (!s->mig_restore_pending) {
        return;
    }

    /*
     * Consume the payload whichever way this goes, so that a failure cannot
     * leave a stale one behind to be applied by a later run state change.
     */
    s->mig_restore_pending = false;

    if (!s->core) {
        error_setg(&local_err, "amd-viommu: migrated vIOMMU state arrived but "
                   "no vIOMMU was allocated to restore it onto");
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    if (!s->mig_supported) {
        error_setg(&local_err,
                   "amd-viommu: the host IOMMU cannot restore vIOMMU state");
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    if (amdvi_mig_replay_hw_queues(s, &local_err)) {
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    len = s->mig_blob_len;
    if (!iommufd_backend_viommu_mig_state(s->iommufd, s->core->viommu_id,
                                          IOMMU_VIOMMU_MIG_OP_RESTORE, 0,
                                          &s->mig_hdr, s->mig_blob, &len, NULL,
                                          &local_err)) {
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    /*
     * Before COMMIT_RESTORE so the flush it performs walks a gdomid_array that
     * the rebuilt nested domains have already populated.
     */
    if (!amdvi_dev_replay_nested(s, &local_err)) {
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    if (!amdvi_mig_replay_ext_int(s, &local_err)) {
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    /*
     * COMMIT_RESTORE arms the queues and flushes the caches, so anything else
     * the destination has to replay onto this vIOMMU belongs before this point.
     */
    if (!iommufd_backend_viommu_mig_state(s->iommufd, s->core->viommu_id,
                                          IOMMU_VIOMMU_MIG_OP_COMMIT_RESTORE,
                                          0, NULL, NULL, NULL, NULL,
                                          &local_err)) {
        amdvi_mig_restore_failed(s, local_err);
        goto out;
    }

    trace_amdvi_mig_restored(s->core->viommu_id, s->mig_blob_len);

out:
    amdvi_mig_payload_reset(s);
}

/*
 * Resume runs here rather than in the handler below so that it lands before
 * VFIO's own prepare callback lets the endpoints issue DMA; see the priority
 * comment at the top of this file.
 */
static void amd_viommu_vm_state_change_prepare(void *opaque,
                                               bool running, RunState state)
{
    AMDVIState *s = opaque;

    if (state == RUN_STATE_RUNNING) {
        amd_viommu_state_change_running(s);
    }
}

static void amd_viommu_vm_state_change(void *opaque,
                                        bool running, RunState state)
{
    AMDVIState *s = opaque;

    switch (state) {
    case RUN_STATE_FINISH_MIGRATE:
    case RUN_STATE_SAVE_VM:
        /*
         * The endpoints behind this vIOMMU are stopped before this runs, since
         * they sort after it by handler priority, so the only DMA left to drain
         * is what the guest already queued.
         *
         * savevm also passes through SAVE_VM; live migration uses
         * FINISH_MIGRATE.  The iommufd payload is parked for VMState in
         * amdvi_mig_save(), which the iommufd-mig subsection then serializes.
         */
        amdvi_mig_save(s);
        break;
    case RUN_STATE_SHUTDOWN:
        /*
         * Teardown destroys the iommufd vIOMMU, its vDEVICEs and the hardware
         * queues.  It must not run while a migration is in flight: on the
         * source that state still has to be saved, and on the destination it
         * would race the restore sequence.  Only RUN_STATE_SHUTDOWN is handled
         * here, which is what keeps teardown away from the migration run
         * states, so catch any future path that breaks the assumption.
         */
        assert(!migration_is_running());
        amd_viommu_state_change_shutdown(s);
        break;
    default:
        break;
    }
}


static void amdvi_set_quad(AMDVIState *s, hwaddr addr, uint64_t val,
                           uint64_t romask, uint64_t w1cmask)
{
    stq_le_p(&s->mmior[addr], val);
    stq_le_p(&s->romask[addr], romask);
    stq_le_p(&s->w1cmask[addr], w1cmask);
}

static uint64_t amdvi_readq(AMDVIState *s, hwaddr addr)
{
    return ldq_le_p(&s->mmior[addr]);
}

/* external write */
static void amdvi_writew(AMDVIState *s, hwaddr addr, uint16_t val)
{
    uint16_t romask = lduw_le_p(&s->romask[addr]);
    uint16_t w1cmask = lduw_le_p(&s->w1cmask[addr]);
    uint16_t oldval = lduw_le_p(&s->mmior[addr]);
    stw_le_p(&s->mmior[addr],
            ((oldval & romask) | (val & ~romask)) & ~(val & w1cmask));
}

static void amdvi_writel(AMDVIState *s, hwaddr addr, uint32_t val)
{
    uint32_t romask = ldl_le_p(&s->romask[addr]);
    uint32_t w1cmask = ldl_le_p(&s->w1cmask[addr]);
    uint32_t oldval = ldl_le_p(&s->mmior[addr]);
    stl_le_p(&s->mmior[addr],
            ((oldval & romask) | (val & ~romask)) & ~(val & w1cmask));
}

static void amdvi_writeq(AMDVIState *s, hwaddr addr, uint64_t val)
{
    uint64_t romask = ldq_le_p(&s->romask[addr]);
    uint64_t w1cmask = ldq_le_p(&s->w1cmask[addr]);
    uint32_t oldval = ldq_le_p(&s->mmior[addr]);
    stq_le_p(&s->mmior[addr],
            ((oldval & romask) | (val & ~romask)) & ~(val & w1cmask));
}

static void amd_viommu_intcapxt_decode(uint64_t val, uint32_t *vcpu_id,
                                       uint8_t *vector)
{
    uint32_t dest_lo = (val >> 8) & 0xffffff;
    uint32_t dest_hi = (val >> 56) & 0xff;

    *vcpu_id = (dest_hi << 24) | dest_lo;
    *vector = (val >> 32) & 0xff;
}

static int amd_viommu_ext_int_remap(AMDVIState *s, uint32_t type, uint64_t val)
{
    uint32_t vcpu_id;
    uint8_t vector;
    int kvmfd;

    if (!val) {
        return 0;
    }

    if (!s->core) {
        return -EINVAL;
    }

    amd_viommu_intcapxt_decode(val, &vcpu_id, &vector);

    kvmfd = kvm_vmfd(kvm_state);
    if (kvmfd < 0) {
        error_report("%s: no KVM VM fd", __func__);
        return -EINVAL;
    }

    return iommufd_viommu_ext_int_remap(s->core, type, kvmfd, vcpu_id, vector);
}

static uint64_t amd_viommu_dte_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t val = 0;
    AMDVIState *s = opaque;

    if (size ==  2) {
        val = lduw_le_p(&s->devtab[offset]);
    } else if (size == 4) {
        val = ldl_le_p(&s->devtab[offset]);
    } else if (size == 8) {
        val = ldq_le_p(&s->devtab[offset]);
    }

    return val;
}

static AMDIOMMUFDDevice *amd_viommu_get_device_from_bdf(AMDVIState *s, uint16_t bus, uint16_t devfn)
{
    AMDIOMMUFDDevice *amd_idev;
    struct AMDVI_dte_key *key;
    GHashTableIter as_it;

    g_hash_table_iter_init(&as_it, s->amd_iommufd_dev_hash);

    while (g_hash_table_iter_next(&as_it, (void **)&key, (void **)&amd_idev)) {
        if (pci_bus_num(key->bus) == bus && key->devfn == devfn)
            return amd_idev;
    }
    return NULL;
}

static int amd_viommu_update_gcr3(AMDVIState *s, AMDIOMMUFDDevice *dev,
				  uint16_t dev_id, uint64_t *dte)
{
    int ret;
    uint32_t hwpt_id, old_hwpt;
    Error *local_err = NULL;
    struct iommu_hwpt_amd_guest hwpt;
    VFIODevice *vdev = dev->hiod->agent;

    hwpt.dte[0] = dte[0];
    hwpt.dte[1] = dte[1];
    hwpt.dte[2] = dte[2];
    hwpt.dte[3] = dte[3];

    /*
     * TODO:
     *  - Destroy already allocated nested page table?
     *  - Detach hwpt/destroy nested domain unset_iommu_device path as well?
     */
    /* Calling drivers/iommu/amd/viommu.c: _amd_viommu_alloc_domain_nested() */
    ret = iommufd_backend_alloc_hwpt(s->iommufd,
                                     vdev->idev.dev_id,
                                     s->core->viommu_id,
                                     0,
                                     IOMMU_HWPT_DATA_AMD_GUEST,
                                     sizeof(hwpt),
                                     &hwpt,
                                     &hwpt_id,
                                     &local_err);
    if (!ret)
        return -EINVAL;


    old_hwpt = dev->v2_hwpt_id;
    dev->v2_hwpt_id = hwpt_id;

    ret = iommufd_device_attach_hwpt(&vdev->idev, hwpt_id);

    if (old_hwpt) {
        iommufd_backend_free_id(s->iommufd, old_hwpt);
    }

    return ret;
}

static void amd_viommu_dte_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *dev;
    AMDVI_dte_info *dte_info;
    bool v = false;
    uint64_t dte[4];
    uint64_t offset0 = 0, offset1 = 0, offset2 = 0, offset3 = 0;
    uint32_t devid;

    /* Storing value in the devtab */
    if (size ==  2) {
        stw_le_p(&s->devtab[offset], val);
    } else if (size == 4) {
        stl_le_p(&s->devtab[offset], val);
    } else if (size == 8) {
        stq_le_p(&s->devtab[offset], val);
    }

    /*
     * Calculate devid from offset using DTE size 0x20. Note that this is offset
     * from the devid from primary bus. See amd_viommu_handle_dev_tab_mmio_write().
     */
    devid = (pci_bus_num(s->root_bus) << 8) + (offset >> 5);

    if (devid > ((s->last_bus_nr << 8) | 0xFF)) {
        error_printf("%s: devid %#x is out of range (%#x)\n", __func__,
                     devid, ((s->last_bus_nr << 8) | 0xFF));
        return;
    }

    dte_info = &s->dte_info[devid];
    dte_info->last = dte_info->curr;
    dte_info->curr = (offset % 0x20) >> 3;

    /*
     * Note:
     * For now, we only care about the case when writing to
     * DTE[1] (for DomainID, GCR3 Table Root Pointer)
     * DTE[2] (for GuestPagingMode).
     *
     * FIXME: Needs to handle 128-bit
     */
    if (offset % 0x20 == 0) {
	offset3 = offset + 0x18;
	offset2 = offset + 0x10;
	offset1 = offset + 0x8;
	offset0 = offset;
    } else if (offset % 0x20 == 0x8) {
	offset3 = offset + 0x10;
	offset2 = offset + 0x8;
	offset1 = offset;
	offset0 = offset - 0x8;
    } else if (offset % 0x20 == 0x10) {
	offset3 = offset + 0x8;
	offset2 = offset;
	offset1 = offset - 0x8;
	offset0 = offset - 0x10;
    } else if (offset % 0x20 == 0x18) {
	offset3 = offset;
	offset2 = offset - 0x8;
	offset1 = offset - 0x10;
	offset0 = offset - 0x18;
    }

    dte[0] = amd_viommu_dte_read(opaque, offset0, size);
    dte[1] = amd_viommu_dte_read(opaque, offset1, size);
    dte[2] = amd_viommu_dte_read(opaque, offset2, size);
    dte[3] = amd_viommu_dte_read(opaque, offset3, size);

    v = dte[0] & 0x1ULL;

    /*
     * Handle cases:
     * 1. Writing DTE[1] then DTE[0]
     * 2. Writing DTE[0] then DTE[1] since
     *    - Linux 6.12 and older does not guarantee the ordering.
     *    - Linux 6.13 and later uses cmpxchg16, which starts from
     *      least significant bit.
     */
    if ((dte_info->last == 1 && dte_info->curr == 0) ||
        (dte_info->last == 0 && dte_info->curr == 1)) {
        /*
         * Update GCR3 when V and GV is set, and GCR3 is updated.
         */
        if (!v || (dte_info->dte0 == dte[0] && dte_info->dte1 == dte[1]))
            return;

        int ret;

        /* FIXME: Currently, we iterate through all devices instead of g_hash_table_lookup()
         * since we cannot get PCIBus from bus number to use it as key
         *
         * TODO: Use bus number itsecd4482c6017elf in key as bus number is used in comparision
         */
        dev = amd_viommu_get_device_from_bdf(s, devid >> 8, devid & 0xFF);
        if (!dev) {
            /* With newer multiple IOMMU support we publish buses in IVRS
             * This means that kernel will try to program DTE for qemu buses
             * We can safely ignore these DTE writes.
             */
            return;
        }

        /* Write everything */
        ret = amd_viommu_update_gcr3(s, dev, devid, dte);
        if (ret)
            return;

        dte_info->dte0 = dte[0];
        dte_info->dte1 = dte[1];

        /* clear dte tracking */
	dte_info->last = -1;
	dte_info->curr = -1;
    }
}

static const MemoryRegionOps dte_ops = {
    .read = amd_viommu_dte_read,
    .write = amd_viommu_dte_write,
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

static void cleanup_devtab(AMDVIState *s)
{
    memory_region_del_subregion(get_system_memory(), &s->devtab_mr);
    object_unparent(OBJECT(&s->devtab_mr));
    free(s->devtab);
    s->devtab = NULL;
    s->devtab_base = 0;
    s->devtab_len = 0;
    s->devtab_size = 0;

    /* A table being torn down leaves any parked migration copy of it stale. */
    g_free(s->devtab_mig);
    s->devtab_mig = NULL;
    s->devtab_mig_len = 0;
}

/*
 * Build the device table shadow and its IO region from AMDVI_MMIO_DEVICE_TABLE.
 *
 * Split out of the register write handler so the restore path can share it: a
 * migrated guest programmed that register on the source and never writes it
 * again, but the register itself crosses in mmior[], so the destination can
 * derive the same base, size and length from it rather than carrying them.
 */
static void amdvi_devtab_setup(AMDVIState *s)
{
    char name[30];
    uint64_t offset, req_size;
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_DEVICE_TABLE);

    snprintf(name, 30, "%s-%02u", "amd-iommu-devtab", s->iommu.index);

    /* Free up previously allocated table */
    if (s->devtab) {
        cleanup_devtab(s);
    }

    s->devtab_base = (val & AMDVI_MMIO_DEVTAB_BASE_MASK);

    /* Offset to the first entry in DTE to be set up for listener */
    offset = s->devtab_base + ((pci_bus_num(s->root_bus) << 8) * 0x20);

    /* The devtab_size is the size specifyied by guest indicated is (n + 1) * 4 Kbytes. */
    s->devtab_size = ((val & AMDVI_MMIO_DEVTAB_SIZE_MASK) + 1) << 12;

    /*
     * The devtab_len is calculated based on the reported bus information
     * for this IOMMU
     */
    s->devtab_len = ((s->last_bus_nr - pci_bus_num(s->root_bus) + 1) << 8);
    s->devtab_len *= 0x20;

    /* The size is calculated from the space before the bus + devtab_len */
    req_size = ((pci_bus_num(s->root_bus) << 8) * 0x20) + s->devtab_len;
    if (req_size > s->devtab_size) {
         fprintf(stderr, "%s: Invalid devtab size %#lx (required %#lx)\n", __func__,
                 s->devtab_size, req_size);
         exit(-EINVAL);
    }

    /* Only allocate devtab storage for the specified bus range */
    s->devtab = g_malloc0(s->devtab_len);
    if (!s->devtab)
        exit(-ENOMEM);

    memset(s->devtab, 0, s->devtab_len);

    /*
     * Set up memory listener for IOMMU Device Table for DTEs within
     * the range of devid for this IOMMU only
     */
    memory_region_init_io(&s->devtab_mr, OBJECT(s), &dte_ops, s, name, s->devtab_len);
    memory_region_add_subregion_overlap(get_system_memory(), offset, &s->devtab_mr, 1);
}

static inline void amdvi_handle_devtab_mmio_write(AMDVIState *s)
{
    amdvi_devtab_setup(s);
}

/*
 * Rebuild the device table on the destination and refill it with the contents
 * the source had.
 *
 * The table is an IO region over a heap buffer rather than guest RAM, so
 * nothing else in the migration stream carries it, and the guest can read back
 * DTEs it wrote for devices that were never assigned.
 */
static bool amdvi_devtab_restore(AMDVIState *s, Error **errp)
{
    /* Taken before the setup below, which frees whatever is parked. */
    g_autofree uint8_t *contents = NULL;
    uint32_t len = s->devtab_mig_len;

    /*
     * Keyed on the flag rather than on the parked table itself, because saving
     * leaves one parked on the source too, and a source resuming from a failed
     * migration must not rebuild the table it is already running on.
     */
    if (!s->devtab_restore_pending) {
        return true;
    }
    s->devtab_restore_pending = false;

    contents = g_steal_pointer(&s->devtab_mig);
    s->devtab_mig_len = 0;

    if (!contents) {
        error_setg(errp, "amd-viommu: migrated device table is missing");
        return false;
    }

    amdvi_devtab_setup(s);

    /*
     * Both lengths come from the bus range this IOMMU serves, so a mismatch
     * means source and destination were given different PCI topologies and the
     * DTEs would land at the wrong devids.
     */
    if (s->devtab_len != len) {
        error_setg(errp, "amd-viommu: migrated device table is %u bytes but "
                   "this host's bus range needs %" PRIu64, len, s->devtab_len);
        return false;
    }

    memcpy(s->devtab, contents, len);

    trace_amdvi_mig_devtab_restored(s->devtab_base, s->devtab_len);

    return true;
}

static inline void amdvi_handle_control_write(AMDVIState *s)
{
    unsigned long ctrl = amdvi_readq(s, AMDVI_MMIO_CONTROL);

    /* TODO:
     * Need to detect enable bits and allocate HW queues accordingly.
     * 1. Command Buffer:
     *    - CmdBufEn (bit 12)
     *    - ComWaitIntEn (bit 4)
     * 2. Event Log:
     *    - EventLogEn (bit 2)
     *    - EventIntEn (bit 3)
     * 3. PPR Log:
     *    - PPRLogEn (bit 13)
     *    - PprIntEn (bit 14)
     *    - PPREn (bit15)
     *    - PprAutoRspEn (bit 39)
     *    - BlkStopMrken (bit 41)
     *    - PprAutoRspAon (bit 42)
     */
    if (ctrl & (1 << 12)) {
        amdvi_handle_cmdbase_write(s, ctrl, false);
    }
    if (ctrl & (1 << 2)) {
        amdvi_handle_evtbase_write(s, ctrl, false);
    }
    if (ctrl & (1 << 13)) {
        amdvi_handle_pprbase_write(s, ctrl, false);
    }
}

/*
 * Allocate the hardware queue described by the command buffer registers.
 *
 * With @masked the queue is allocated with its enable bits cleared and the
 * dedup cache is ignored, which is what the restore path needs: the queue has
 * to exist for MIG_STATE RESTORE to write head, tail and status into, but must
 * not run until COMMIT_RESTORE arms it (D-015).  The cache still records the
 * flags the guest programmed, because those are the ones COMMIT_RESTORE
 * applies, and a later identical guest write must dedup rather than tear the
 * restored queue down.
 */
static void amdvi_handle_cmdbase_write(AMDVIState *s, unsigned long ctrl,
                                       bool masked)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_COMMAND_BASE);
    uint64_t addr = (val & 0xFFFFFFFFFF000ULL);
    uint64_t len = (val >> 56) & 0xF;
    uint32_t flags = 0;

    if (ctrl & (1UL << 12))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_CMDBUF_EN;
    if (ctrl & (1UL << 4))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_COMWAIT_EN;

    if (!masked && s->cmdbuf_hwq &&
        s->cmdbuf_hwq_reg_addr == addr &&
        s->cmdbuf_hwq_reg_len == len &&
        s->cmdbuf_hwq_reg_flags == flags) {
        return;
    }

    if (s->cmdbuf_hwq) {
        amdvi_free_cmdbuf_hwq(s);
    }

    s->cmdbuf_hwq = iommufd_viommu_alloc_hw_queue(s->core,
                                          IOMMU_HW_QUEUE_TYPE_AMD_CMD,
                                          0, addr, len, masked ? 0 : flags);
    if (!s->cmdbuf_hwq) {
        error_report("%s: failed to allocate command buffer\n", __func__);
        return;
    }
    s->cmdbuf_hwq_reg_addr = addr;
    s->cmdbuf_hwq_reg_len = len;
    s->cmdbuf_hwq_reg_flags = flags;
}

/* See amdvi_handle_cmdbase_write() for what @masked means. */
static void amdvi_handle_evtbase_write(AMDVIState *s, unsigned long ctrl,
                                       bool masked)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_EVENT_BASE);
    uint64_t addr = (val & 0xFFFFFFFFFF000ULL);
    uint64_t len = (val >> 56) & 0xF;
    uint32_t flags = 0;

    if (ctrl & (1UL << 2))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_EVT_LOG_EN;
    if (ctrl & (1UL << 3))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_EVT_INT_EN;

    if (!masked && s->evtlog_hwq &&
        s->evtlog_hwq_reg_addr == addr &&
        s->evtlog_hwq_reg_len == len &&
        s->evtlog_hwq_reg_flags == flags) {
        return;
    }

    if (s->evtlog_hwq) {
        amdvi_free_evtlog_hwq(s);
    }

    s->evtlog_hwq = iommufd_viommu_alloc_hw_queue(s->core,
                                          IOMMU_HW_QUEUE_TYPE_AMD_EVT,
                                          0, addr, len, masked ? 0 : flags);
    if (!s->evtlog_hwq) {
        error_report("%s: failed to allocate event log\n", __func__);
        return;
    }
    s->evtlog_hwq_reg_addr = addr;
    s->evtlog_hwq_reg_len = len;
    s->evtlog_hwq_reg_flags = flags;
}

/* See amdvi_handle_cmdbase_write() for what @masked means. */
static void amdvi_handle_pprbase_write(AMDVIState *s, unsigned long ctrl,
                                       bool masked)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_PPR_BASE);
    uint64_t addr = (val & 0xFFFFFFFFFF000ULL);
    uint64_t len = (val >> 56) & 0xF;
    uint32_t flags = 0;

    if (ctrl & (1UL << 13))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPRLOG_EN;
    if (ctrl & (1UL << 14))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPRINT_EN;
    if (ctrl & (1UL << 15))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPR_EN;
    if (ctrl & (1UL << 39))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPR_AUTO_RSP_EN;
    if (ctrl & (1UL << 41))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_BLKSTOPMRK_EN;
    if (ctrl & (1UL << 42))
        flags |= IOMMU_HW_QUEUE_FLAG_AMD_PPR_AUTO_RSP_AON;

    if (!masked && s->pprlog_hwq &&
        s->pprlog_hwq_reg_addr == addr &&
        s->pprlog_hwq_reg_len == len &&
        s->pprlog_hwq_reg_flags == flags) {
        return;
    }

    if (s->pprlog_hwq) {
        amdvi_free_pprlog_hwq(s);
    }

    s->pprlog_hwq = iommufd_viommu_alloc_hw_queue(s->core,
                                          IOMMU_HW_QUEUE_TYPE_AMD_PPR,
                                          0, addr, len, masked ? 0 : flags);
    if (!s->pprlog_hwq) {
        error_report("%s: failed to allocate ppr log\n", __func__);
        return;
    }
    s->pprlog_hwq_reg_addr = addr;
    s->pprlog_hwq_reg_len = len;
    s->pprlog_hwq_reg_flags = flags;
}

static inline int amdvi_handle_xt_event_int_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_XT_EVENT_INT);

    return amd_viommu_ext_int_remap(s, IOMMU_VIOMMU_EXT_INT_EVENT, val);
}

static inline int amdvi_handle_xt_ppr_int_write(AMDVIState *s)
{
    uint64_t val = amdvi_readq(s, AMDVI_MMIO_XT_PPR_INT);

    return amd_viommu_ext_int_remap(s, IOMMU_VIOMMU_EXT_INT_PPR, val);
}

/* FIXME: something might go wrong if System Software writes in chunks
 * of one byte but linux writes in chunks of 4 bytes so currently it
 * works correctly with linux but will definitely be busted if software
 * reads/writes 8 bytes
 */

static void amdvi_mmio_reg_write(AMDVIState *s, unsigned size, uint64_t val,
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

static void amdvi_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    AMDVIState *s = opaque;
    unsigned long offset = addr & 0x07;

    /* The guest has enumerated PCI by now; see vdev_alloc_all(). */
    vdev_alloc_all(s);

    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amdvi_mmio_write("error: addr outside region: max ",
                (uint64_t)AMDVI_MMIO_SIZE, size, val, offset);
        return;
    }

    switch (addr & ~0x07) {
    case AMDVI_MMIO_CONTROL:
        amdvi_mmio_reg_write(s, size, val, addr);

        /* FIXME - make sure System Software has finished writing incase
         * it writes in chucks less than 8 bytes in a robust way.As for
         * now, this hacks works for the linux driver
         */
        if (offset || (size == 8)) {
            amdvi_handle_control_write(s);
        }
        break;
    case AMDVI_MMIO_DEVICE_TABLE:
        amdvi_mmio_reg_write(s, size, val, addr);
       /*  set device table address
        *   This also suffers from inability to tell whether software
        *   is done writing
        */
        if (offset || (size == 8)) {
            amdvi_handle_devtab_mmio_write(s);
        }
        break;
    case AMDVI_MMIO_COMMAND_BASE:
        amdvi_mmio_reg_write(s, size, val, addr);
        break;
    case AMDVI_MMIO_EVENT_BASE:
        amdvi_mmio_reg_write(s, size, val, addr);
        break;
    case AMDVI_MMIO_PPR_BASE:
        amdvi_mmio_reg_write(s, size, val, addr);
        break;
    case AMDVI_MMIO_XT_EVENT_INT:
        amdvi_mmio_reg_write(s, size, val, addr);
        if (!offset && size == 8) {
            amdvi_handle_xt_event_int_write(s);
        }
        break;
    case AMDVI_MMIO_XT_PPR_INT:
        amdvi_mmio_reg_write(s, size, val, addr);
        if (!offset && size == 8) {
            amdvi_handle_xt_ppr_int_write(s);
        }
        break;
    }
}

static uint64_t amdvi_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AMDVIState *s = opaque;
    uint64_t val = 0;

    if (addr + size > AMDVI_MMIO_SIZE) {
        trace_amdvi_mmio_read_invalid(AMDVI_MMIO_SIZE, addr, size);
        return (uint64_t)-1;
    }

    if (size == 2) {
        val = lduw_le_p(&s->mmior[addr]);
    } else if (size == 4) {
        val = ldl_le_p(&s->mmior[addr]);
    } else if (size == 8) {
        val = ldq_le_p(&s->mmior[addr]);
    }

    return val;
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
 * TODO: add command line arguments
 * TODO: This patch is big so better to split it
 *  --- Introducing address spaces
 *  --- Updating header with SUPPORTED_EFR
 *  --- Implementing set/unset iommu device
 */
static void _build_efr_guest_translation(VendorCaps *caps,
                                         uint64_t *efr, uint64_t  *efr2)
{
    *efr = (caps->amd.efr & SUPPORTED_EFR);
    *efr2 = 0ULL;
}

static bool amd_viommu_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                   HostIOMMUDevice *hiod, Error **errp)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *amd_idev;
    struct AMDVI_dte_key *new_key;
    struct AMDVI_dte_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    VendorCaps *caps = &hiod->caps.vendor_caps;

    assert(hiod);
    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (g_hash_table_lookup(s->amd_iommufd_dev_hash, &key)) {
        error_setg(errp, "Host IOMMU device already exist");
        return false;
    }

    if (hiod->caps.type != IOMMU_HW_INFO_TYPE_AMD) {
        error_report("IOMMU hardware type %#x is not compatible!!!", hiod->caps.type);
        return false;
    }

    new_key = g_malloc(sizeof(*new_key));
    new_key->bus = bus;
    new_key->devfn = devfn;

    amd_idev = g_malloc0(sizeof(AMDIOMMUFDDevice));
    amd_idev->iommu_state = s;
    amd_idev->hiod = hiod;
    amd_idev->v2_hwpt_id =  0;
    amd_idev->passthrough_hwpt_id = 0;

    /* Held for as long as the entry is, and dropped by amd_idev_destroy(). */
    object_ref(hiod);
    g_hash_table_insert(s->amd_iommufd_dev_hash, new_key, amd_idev);

    /* Use iommufd handler opened by device */
    s->iommufd = idev->iommufd;
    _build_efr_guest_translation(caps, (uint64_t*) &s->hwinfo.efr,
                                 (uint64_t*) &s->hwinfo.efr2);

    return true;
}

static void amd_viommu_unset_iommu_device(PCIBus *bus, void *opaque,
                                     int devfn)
{
    AMDVIState *s = opaque;
    struct AMDVI_dte_key key = {
        .bus = bus,
        .devfn = devfn,
    };

    if (!g_hash_table_remove(s->amd_iommufd_dev_hash, &key)) {
        return;
    }

    /*
     * The preconditions are about the set of assigned devices, and one just
     * left it.  A device that was the reason migration was refused takes the
     * refusal with it.
     */
    amdvi_mig_blocker_update(s);
}

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

static uint64_t amd_viommu_get_viommu_flags(void *opaque)
{
    return VIOMMU_FLAG_WANT_NESTING_PARENT;
}

static const PCIIOMMUOps amdvi_iommu_ops = {
    .get_address_space = amd_viommu_get_address_space,
    .set_iommu_device = amd_viommu_set_iommu_device,
    .unset_iommu_device = amd_viommu_unset_iommu_device,
    .get_x86_iommu = amd_viommu_get_x86_iommu,
    .get_viommu_flags = amd_viommu_get_viommu_flags,
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

/*
 * An instance is identified by the bus range it serves rather than by
 * x86_iommu->index, which is handed out from a counter in realize order and
 * says nothing a user configured.  The range is what makes two instances
 * different from each other, and it is what sizes the guest device table, so
 * comparing it also catches a destination whose last-bus-nr simply disagrees
 * with the source's.
 */
static int amdvi_identity_pre_save(void *opaque)
{
    AMDVIState *s = opaque;

    s->mig_first_bus_nr = pci_bus_num(s->root_bus);
    s->mig_last_bus_nr = s->last_bus_nr;

    return 0;
}

static bool amdvi_identity_post_load(void *opaque, int version_id, Error **errp)
{
    AMDVIState *s = opaque;

    if (s->mig_first_bus_nr != pci_bus_num(s->root_bus) ||
        s->mig_last_bus_nr != s->last_bus_nr) {
        error_setg(errp, "amd-viommu: migrated vIOMMU serves buses %02x-%02x "
                   "but this one serves %02x-%02x", s->mig_first_bus_nr,
                   s->mig_last_bus_nr, pci_bus_num(s->root_bus),
                   s->last_bus_nr);
        return false;
    }

    return true;
}

/*
 * Carried by every instance, including the only one of a single-vIOMMU guest:
 * a destination that disagrees about the bus range is worth refusing however
 * many vIOMMUs are present, and the range is the whole of what travels.
 */
static const VMStateDescription vmstate_amd_viommu_identity = {
    .name = "amd-viommu/identity",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = amdvi_identity_pre_save,
    .post_load_errp = amdvi_identity_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mig_first_bus_nr, AMDVIState),
        VMSTATE_UINT32(mig_last_bus_nr, AMDVIState),
        VMSTATE_END_OF_LIST()
    }
};

/* Nothing to carry when the kernel gave us no payload to hand back. */
static bool amdvi_iommufd_mig_needed(void *opaque)
{
    AMDVIState *s = opaque;

    return s->mig_blob_len > 0;
}

/*
 * Once the payload is on the wire (or in the snapshot file), drop the
 * in-memory copy.  For savevm the source resumes, so hand the quiesced
 * vIOMMU back to the guest; migration leaves it quiesced until shutdown.
 */
static int amdvi_iommufd_mig_post_save(void *opaque)
{
    AMDVIState *s = opaque;

    if (runstate_check(RUN_STATE_SAVE_VM)) {
        amdvi_mig_abort(s);
    }
    amdvi_mig_payload_reset(s);
    return 0;
}

/*
 * Uses .post_load_errp rather than .post_load so that the reason a payload was
 * rejected travels with the failure, instead of being printed separately from
 * the error the migration ends up reporting.
 */
static bool amdvi_iommufd_mig_post_load(void *opaque, int version_id,
                                        Error **errp)
{
    AMDVIState *s = opaque;

    /*
     * The echoes travelled as scalars and the header as raw bytes, so a
     * disagreement means the two describe different payloads. Refuse here
     * rather than hand the kernel a header its own payload contradicts.
     */
    if (s->mig_hdr_version != s->mig_hdr.blob_format_version) {
        error_setg(errp, "amd-viommu: migrated payload header format %u does "
                   "not match its echo %u", s->mig_hdr.blob_format_version,
                   s->mig_hdr_version);
        return false;
    }

    if (s->mig_hdr_payload_size != s->mig_hdr.payload_size) {
        error_setg(errp, "amd-viommu: migrated payload header size %u does not "
                   "match its echo %u", s->mig_hdr.payload_size,
                   s->mig_hdr_payload_size);
        return false;
    }

    if (s->mig_hdr.payload_size != s->mig_blob_len) {
        error_setg(errp, "amd-viommu: migrated payload header describes %u "
                   "bytes but %u bytes arrived", s->mig_hdr.payload_size,
                   s->mig_blob_len);
        return false;
    }

    /*
     * The vIOMMU does not exist yet, so the payload cannot be handed to the
     * kernel from here; it is applied once the run state reaches RUNNING and
     * the vIOMMU has been allocated.
     */
    s->mig_restore_pending = true;
    return true;
}

/*
 * Carrying the header as raw bytes makes its size part of the migration wire
 * format, so a UAPI that grew it would break compatibility between QEMU
 * versions silently. The kernel promises 64 bytes forever; hold it to that.
 */
QEMU_BUILD_BUG_ON(sizeof(struct iommu_viommu_mig_header) != 64);

/*
 * The amd-viommu/ prefix is required, not cosmetic: vmstate_subsection_load()
 * returns success when a name does not match the parent's prefix, so a
 * mis-named subsection would be skipped and the destination would resume on an
 * unconfigured vIOMMU.
 */
static const VMStateDescription vmstate_amd_viommu_iommufd_mig = {
    .name = "amd-viommu/iommufd-mig",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = amdvi_iommufd_mig_needed,
    .post_save = amdvi_iommufd_mig_post_save,
    .post_load_errp = amdvi_iommufd_mig_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mig_hdr_version, AMDVIState),
        VMSTATE_UINT32(mig_hdr_payload_size, AMDVIState),
        /* Sizes @mig_blob's allocation on load, so it has to precede it. */
        VMSTATE_UINT32(mig_blob_len, AMDVIState),
        VMSTATE_BUFFER_UNSAFE(mig_hdr, AMDVIState, 0,
                              sizeof(struct iommu_viommu_mig_header)),
        VMSTATE_VBUFFER_ALLOC_UINT32(mig_blob, AMDVIState, 0, NULL,
                                     mig_blob_len),
        VMSTATE_END_OF_LIST()
    }
};

static bool amdvi_devtab_needed(void *opaque)
{
    AMDVIState *s = opaque;

    return s->devtab && s->devtab_len;
}

static int amdvi_devtab_pre_save(void *opaque)
{
    AMDVIState *s = opaque;

    g_free(s->devtab_mig);
    s->devtab_mig = g_memdup2(s->devtab, s->devtab_len);
    s->devtab_mig_len = s->devtab_len;

    return 0;
}

static int amdvi_devtab_post_save(void *opaque)
{
    AMDVIState *s = opaque;

    /* The source has no use for its own snapshot once it is on the wire. */
    g_free(s->devtab_mig);
    s->devtab_mig = NULL;
    s->devtab_mig_len = 0;

    return 0;
}

/*
 * The table cannot be applied from here: rebuilding it installs a memory
 * region, and the geometry it is rebuilt from comes from mmior[], which the
 * parent descriptor is still in the middle of loading.
 */
static int amdvi_devtab_post_load(void *opaque, int version_id)
{
    AMDVIState *s = opaque;

    s->devtab_restore_pending = true;

    return 0;
}

static bool amdvi_dev_replay_needed(void *opaque)
{
    AMDVIState *s = opaque;

    /*
     * Checked before pre_save builds the records, so it asks whether there are
     * any devices to describe rather than how many records there are.
     */
    return g_hash_table_size(s->amd_iommufd_dev_hash) > 0;
}

static int amdvi_dev_replay_pre_save(void *opaque)
{
    AMDVIState *s = opaque;
    AMDIOMMUFDDevice *amd_idev;
    struct AMDVI_dte_key *key;
    GHashTableIter it;
    int32_t i = 0;

    g_free(s->dev_replay);
    s->dev_replay_count = g_hash_table_size(s->amd_iommufd_dev_hash);
    s->dev_replay = g_new0(AMDVI_dev_replay, s->dev_replay_count);

    g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

    while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
        uint32_t devid = (pci_bus_num(key->bus) << 8) | key->devfn;
        AMDVI_dev_replay *rec = &s->dev_replay[i++];
        AMDVI_dte_info *info = &s->dte_info[devid];

        /* The table starts at the primary bus, so devid is not its index. */
        uint64_t off = (devid - (pci_bus_num(s->root_bus) << 8)) << 5;

        rec->devid = devid;
        rec->had_nested = amd_idev->v2_hwpt_id != 0;
        rec->dte[0] = info->dte0;
        rec->dte[1] = info->dte1;
        rec->last = info->last;
        rec->curr = info->curr;

        /*
         * Only DTE[0] and DTE[1] are tracked as the domain's inputs, so the
         * other two come from the table, which crosses alongside these records.
         * A guest that never programmed a table has no nested domain either,
         * so leaving them zero costs nothing.
         */
        if (s->devtab && off + 0x20 <= s->devtab_len) {
            rec->dte[2] = ldq_le_p(&s->devtab[off + 0x10]);
            rec->dte[3] = ldq_le_p(&s->devtab[off + 0x18]);
        }
    }

    return 0;
}

static int amdvi_dev_replay_post_save(void *opaque)
{
    AMDVIState *s = opaque;

    g_free(s->dev_replay);
    s->dev_replay = NULL;
    s->dev_replay_count = 0;

    return 0;
}

/*
 * Rejected here rather than at replay time, because a record naming a device
 * this destination was not given means the two sides were configured
 * differently and the guest would come up with a device it cannot drive.
 */
static bool amdvi_dev_replay_post_load(void *opaque, int version_id,
                                       Error **errp)
{
    AMDVIState *s = opaque;
    int32_t i;

    for (i = 0; i < s->dev_replay_count; i++) {
        AMDVI_dev_replay *rec = &s->dev_replay[i];

        if (!amd_viommu_get_device_from_bdf(s, rec->devid >> 8,
                                            rec->devid & 0xFF)) {
            error_setg(errp, "amd-viommu: migrated device %02x:%02x.%x is not "
                       "assigned on this host", rec->devid >> 8,
                       PCI_SLOT(rec->devid & 0xFF),
                       PCI_FUNC(rec->devid & 0xFF));
            return false;
        }
    }

    s->dev_replay_pending = true;

    return true;
}

static const VMStateDescription vmstate_amd_viommu_dev_replay_entry = {
    .name = "amd-viommu/dev-replay/entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(devid, AMDVI_dev_replay),
        VMSTATE_BOOL(had_nested, AMDVI_dev_replay),
        VMSTATE_UINT64_ARRAY(dte, AMDVI_dev_replay, 4),
        VMSTATE_INT32(last, AMDVI_dev_replay),
        VMSTATE_INT32(curr, AMDVI_dev_replay),
        VMSTATE_END_OF_LIST()
    }
};

/* The amd-viommu/ prefix is required for the same reason as above. */
static const VMStateDescription vmstate_amd_viommu_dev_replay = {
    .name = "amd-viommu/dev-replay",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = amdvi_dev_replay_needed,
    .pre_save = amdvi_dev_replay_pre_save,
    .post_save = amdvi_dev_replay_post_save,
    .post_load_errp = amdvi_dev_replay_post_load,
    .fields = (const VMStateField[]) {
        /* Sizes the array's allocation on load, so it has to precede it. */
        VMSTATE_INT32(dev_replay_count, AMDVIState),
        VMSTATE_STRUCT_VARRAY_ALLOC(dev_replay, AMDVIState, dev_replay_count, 0,
                                    vmstate_amd_viommu_dev_replay_entry,
                                    AMDVI_dev_replay),
        VMSTATE_END_OF_LIST()
    }
};

/* The amd-viommu/ prefix is required for the same reason as above. */
static const VMStateDescription vmstate_amd_viommu_devtab = {
    .name = "amd-viommu/devtab",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = amdvi_devtab_needed,
    .pre_save = amdvi_devtab_pre_save,
    .post_save = amdvi_devtab_post_save,
    .post_load = amdvi_devtab_post_load,
    .fields = (const VMStateField[]) {
        /* Sizes @devtab_mig's allocation on load, so it has to precede it. */
        VMSTATE_UINT32(devtab_mig_len, AMDVIState),
        VMSTATE_VBUFFER_ALLOC_UINT32(devtab_mig, AMDVIState, 0, NULL,
                                     devtab_mig_len),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_amd_viommu = {
    .name = "amd-viommu",
    /*
     * Version 1 rather than 0 on purpose.  A QEMU predating this work sets only
     * .name and .unmigratable, so its implicit version is 0 and an incoming
     * stream at version 1 trips its version check.  That check is the only
     * thing protecting that direction, since .unmigratable is consulted when
     * saving and not when loading.
     */
    .version_id = 1,
    .minimum_version_id = 1,
    .priority = MIG_PRI_IOMMU,
    .fields = (const VMStateField[]) {
        /*
         * The whole register file, not the 0x0-0x1FFF the guest usually
         * touches: the high half holds the counter and event registers, and a
         * destination that zeroed them would disagree with a guest that had
         * read them.  w1cmask[] and romask[] stay out because amdvi_init()
         * rebuilds them from the device model rather than from guest state.
         */
        VMSTATE_UINT8_ARRAY(mmior, AMDVIState, AMDVI_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        /* First, so that a payload is never applied to the wrong instance. */
        &vmstate_amd_viommu_identity,
        &vmstate_amd_viommu_devtab,
        &vmstate_amd_viommu_dev_replay,
        &vmstate_amd_viommu_iommufd_mig,
        NULL
    }
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


static uint64_t amd_viommu_extended_feature_register(AMDVIState *s)
{
    return s->hwinfo.efr;
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
    s->get_extended_feature_register = amd_viommu_extended_feature_register;
    s->vdevice_setup = false;

    /* reset MMIO */
    memset(s->mmior, 0, AMDVI_MMIO_SIZE);
    amdvi_set_quad(s, AMDVI_MMIO_EXT_FEATURES, AMD_VIOMMU_DEFAULT_EXT_FEATURES,
            0xffffffffffffffef, 0);
    amdvi_set_quad(s, AMDVI_MMIO_STATUS, 0, 0x98, 0x67);

    for (int i = 0; i < AMDVI_DEVID_MAX; i++) {
	    s->dte_info[i].last = -1;
	    s->dte_info[i].curr = -1;
	    s->dte_info[i].dte0 = ~0ULL;
	    s->dte_info[i].dte1 = ~0ULL;
    }

    qemu_add_vm_change_state_handler_prio_full(
        amd_viommu_vm_state_change, amd_viommu_vm_state_change_prepare,
        NULL, s, AMD_VIOMMU_VM_STATE_PRIO);
}


static guint amdvi_dte_hash(gconstpointer v)
{
    const struct AMDVI_dte_key *key = v;
    guint value = (guint)(uintptr_t)key->bus;

    return (guint)(value << 8 | key->devfn);
}

static gboolean amdvi_dte_equal(gconstpointer v1, gconstpointer v2)
{
    const struct AMDVI_dte_key *key1 = v1;
    const struct AMDVI_dte_key *key2 = v2;

    return (key1->bus == key2->bus) && (key1->devfn == key2->devfn);
}

/*
 * The hash owns the device, so removing the entry is all it takes to give up
 * everything the device held.  The kernel objects are not freed here: they are
 * released while the vIOMMU is being torn down, which is where the iommufd
 * handle they were allocated from is still known to be live.
 */
static void amd_idev_destroy(gpointer v)
{
    AMDIOMMUFDDevice *amd_idev = v;

    object_unref(amd_idev->hiod);
    g_free(amd_idev->core);
    g_free(amd_idev);
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

    s->amd_iommufd_dev_hash = g_hash_table_new_full(amdvi_dte_hash,
                                                    amdvi_dte_equal,
                                                    g_free,
                                                    amd_idev_destroy);

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
    memory_region_init_io(&s->mr_mmio, OBJECT(s), &mmio_mem_ops, s,
                          "amdvi-mmio", 0x2000);
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

static void amd_viommu_unrealize(DeviceState *dev)
{
    AMDVIState *s = AMD_VIOMMU_DEVICE(dev);
    struct AMDVI_dte_key *key;
    GHashTableIter it;
    AMDIOMMUFDDevice *amd_idev;
    Error *local_err = NULL;

    if (s->vf_mmio_page) {
        munmap(s->vf_mmio_page, AMDVI_PAGE_SIZE);
        s->vf_mmio_page = NULL;
    }

    g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

    while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
        HostIOMMUDeviceIOMMUFD *hdev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

        if (!amd_idev->v2_hwpt_id) {
            continue;
        }

        host_iommu_device_iommufd_attach_hwpt(
                hdev,
                amd_idev->passthrough_hwpt_id,
                &local_err);

        iommufd_backend_free_id(s->iommufd, amd_idev->v2_hwpt_id);
        amd_idev->v2_hwpt_id = 0;
    }

}

static void amd_viommu_sysbus_reset(DeviceState *dev)
{
    AMDVIState *s = AMD_VIOMMU_DEVICE(dev);
          struct AMDVI_dte_key *key;
      GHashTableIter it;
      AMDIOMMUFDDevice *amd_idev;
      Error *local_err = NULL;

    if (s->devtab) {
        cleanup_devtab(s);
    }
         g_hash_table_iter_init(&it, s->amd_iommufd_dev_hash);

      while (g_hash_table_iter_next(&it, (void **)&key, (void **)&amd_idev)) {
          HostIOMMUDeviceIOMMUFD *hdev = HOST_IOMMU_DEVICE_IOMMUFD(amd_idev->hiod);

          if (!amd_idev->v2_hwpt_id) {
              continue;
          }

          host_iommu_device_iommufd_attach_hwpt(
                  hdev,
                  amd_idev->passthrough_hwpt_id,
                  &local_err);

          iommufd_backend_free_id(s->iommufd, amd_idev->v2_hwpt_id);
          amd_idev->v2_hwpt_id = 0;
      }
}

static const Property amd_viommu_properties[] = {
    DEFINE_PROP_STRING("pci-id", AMDVIState, pci_id),
    DEFINE_PROP_UINT32("last-bus-nr", AMDVIState, last_bus_nr, 0),
};

static void amd_viommu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *dc_class = X86_IOMMU_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, amd_viommu_sysbus_reset);
    dc->vmsd = &vmstate_amd_viommu;
    dc->hotpluggable = false;
    dc_class->realize = amd_viommu_sysbus_realize;
    dc_class->unrealize = amd_viommu_unrealize;

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
