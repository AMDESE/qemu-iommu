#include "hw/pci/pci.h"
#include "qom/object.h"
#include "hw/i386/x86-iommu.h"

/* functions to steal PCI config space */
typedef struct AMDVIPCIState {
    PCIDevice dev;               /* The PCI device itself        */
    uint32_t capab_offset;       /* capability offset pointer    */
} AMDVIPCIState;

typedef struct AMDIVRSInfo {
    struct AMDVIPCIState *pci;  /* IOMMU PCI device               */
    PCIBus *root_bus;           /* root bus to register IOMMU ops */
    uint64_t base_addr;         /* MMIO region                    */
    uint64_t (*get_extended_feature_register)(X86IOMMUState *s);
} AMDIVRSInfo;


#define GENMASK64(h, l)  (((~0ULL) >> (63 - (h) + (l))) << (l))

#define AMDVI_CAPAB_SIZE              0x18
#define AMDVI_CAPAB_REG_SIZE          0x04

/* Capability registers */
#define AMDVI_CAPAB_BAR_LOW           0x04
#define AMDVI_CAPAB_BAR_HIGH          0x08
#define AMDVI_CAPAB_RANGE             0x0C
#define AMDVI_CAPAB_MISC              0x10

#define AMDVI_CAPAB_SIZE              0x18
#define AMDVI_CAPAB_REG_SIZE          0x04

/* Capability header data */
#define AMDVI_CAPAB_ID_SEC            0xf
#define AMDVI_CAPAB_FLAT_EXT          (1 << 28)
#define AMDVI_CAPAB_EFR_SUP           (1 << 27)
#define AMDVI_CAPAB_FLAG_NPCACHE      (1 << 26)
#define AMDVI_CAPAB_FLAG_HTTUNNEL     (1 << 25)
#define AMDVI_CAPAB_FLAG_IOTLBSUP     (1 << 24)
#define AMDVI_CAPAB_INIT_TYPE         (3 << 16)

#define AMDVI_MAX_GVA_ADDR      (2UL << 5)
#define AMDVI_MAX_PH_ADDR       (40UL << 8)
#define AMDVI_MAX_VA_ADDR       (48UL << 15)

#define AMDVI_MMIO_SIZE               0x4000

/* AMDVI default address */
#define AMDVI_BASE_ADDR 0xfed80000ULL
#define AMDVI_GET_BASE_ADDR(index) \
        (AMDVI_BASE_ADDR + ((index) * AMDVI_MMIO_SIZE))

/* MMIO registers */
#define AMDVI_MMIO_DEVICE_TABLE       0x0000
#define AMDVI_MMIO_COMMAND_BASE       0x0008
#define AMDVI_MMIO_EVENT_BASE         0x0010
#define AMDVI_MMIO_CONTROL            0x0018
#define AMDVI_MMIO_EXCL_BASE          0x0020
#define AMDVI_MMIO_EXCL_LIMIT         0x0028
#define AMDVI_MMIO_EXT_FEATURES       0x0030
#define AMDVI_MMIO_XT_GEN_INTR        0x0170
#define AMDVI_MMIO_COMMAND_HEAD       0x2000
#define AMDVI_MMIO_COMMAND_TAIL       0x2008
#define AMDVI_MMIO_EVENT_HEAD         0x2010
#define AMDVI_MMIO_EVENT_TAIL         0x2018
#define AMDVI_MMIO_STATUS             0x2020
#define AMDVI_MMIO_PPR_BASE           0x0038
#define AMDVI_MMIO_PPR_HEAD           0x2030
#define AMDVI_MMIO_PPR_TAIL           0x2038

#define AMDVI_MMIO_DEVTAB_SIZE_MASK     GENMASK64(8, 0)
#define AMDVI_MMIO_DEVTAB_BASE_MASK     GENMASK64(51, 12)

#define AMDVI_MMIO_DEVTAB_ENTRY_SIZE  32
#define AMDVI_MMIO_DEVTAB_SIZE_UNIT   4096

/* some of this are similar but just for readability */
#define AMDVI_MMIO_CMDBUF_SIZE_BYTE       (AMDVI_MMIO_COMMAND_BASE + 7)
#define AMDVI_MMIO_CMDBUF_SIZE_MASK       0x0f
#define AMDVI_MMIO_CMDBUF_BASE_MASK       GENMASK64(51, 12)
#define AMDVI_MMIO_CMDBUF_HEAD_MASK       GENMASK64(18, 4)
#define AMDVI_MMIO_CMDBUF_TAIL_MASK       GENMASK64(18, 4)

#define AMDVI_MMIO_EVTLOG_SIZE_BYTE       (AMDVI_MMIO_EVENT_BASE + 7)
#define AMDVI_MMIO_EVTLOG_SIZE_MASK       0x0f
#define AMDVI_MMIO_EVTLOG_BASE_MASK       GENMASK64(51, 12)
#define AMDVI_MMIO_EVTLOG_HEAD_MASK       GENMASK64(18, 4)
#define AMDVI_MMIO_EVTLOG_TAIL_MASK       GENMASK64(18, 4)

#define AMDVI_MMIO_PPRLOG_SIZE_BYTE       (AMDVI_MMIO_PPR_BASE + 7)
#define AMDVI_MMIO_PPRLOG_SIZE_MASK       0x0f
#define AMDVI_MMIO_PPRLOG_BASE_MASK       GENMASK64(51, 12)
#define AMDVI_MMIO_PPRLOG_HEAD_MASK       GENMASK64(18, 4)
#define AMDVI_MMIO_PPRLOG_TAIL_MASK       GENMASK64(18, 4)

#define AMDVI_MMIO_EXCL_ENABLED_MASK      (1ULL << 0)
#define AMDVI_MMIO_EXCL_ALLOW_MASK        (1ULL << 1)
#define AMDVI_MMIO_EXCL_LIMIT_MASK        GENMASK64(51, 12)
#define AMDVI_MMIO_EXCL_LIMIT_LOW         0xfff

/* mmio control register flags */
#define AMDVI_MMIO_CONTROL_AMDVIEN        (1ULL << 0)
#define AMDVI_MMIO_CONTROL_HTTUNEN        (1ULL << 1)
#define AMDVI_MMIO_CONTROL_EVENTLOGEN     (1ULL << 2)
#define AMDVI_MMIO_CONTROL_EVENTINTEN     (1ULL << 3)
#define AMDVI_MMIO_CONTROL_COMWAITINTEN   (1ULL << 4)
#define AMDVI_MMIO_CONTROL_CMDBUFLEN      (1ULL << 12)
#define AMDVI_MMIO_CONTROL_GAEN           (1ULL << 17)
#define AMDVI_MMIO_CONTROL_XTEN           (1ULL << 50)
#define AMDVI_MMIO_CONTROL_INTCAPXTEN     (1ULL << 51)

/* MMIO status register bits */
#define AMDVI_MMIO_STATUS_CMDBUF_RUN  (1 << 4)
#define AMDVI_MMIO_STATUS_EVT_RUN     (1 << 3)
#define AMDVI_MMIO_STATUS_COMP_INT    (1 << 2)
#define AMDVI_MMIO_STATUS_EVENT_INT   (1 << 1)
#define AMDVI_MMIO_STATUS_EVT_OVF     (1 << 0)

/* page management constants */
#define AMDVI_PAGE_SHIFT 12
#define AMDVI_PAGE_SIZE  (1ULL << AMDVI_PAGE_SHIFT)

#define AMDVI_PAGE_SHIFT_4K 12

/* Device table entry bits 0:63 */
#define AMDVI_DEV_VALID                   (1ULL << 0)
#define AMDVI_DEV_TRANSLATION_VALID       (1ULL << 1)
#define AMDVI_DEV_MODE_MASK               0x7
#define AMDVI_DEV_MODE_RSHIFT             9
#define AMDVI_DEV_PT_ROOT_MASK            GENMASK64(51, 12)
#define AMDVI_DEV_PT_ROOT_RSHIFT          12
#define AMDVI_DEV_G_TRANSLATION_VALID     (1ULL << 55)
#define AMDVI_DEV_PERM_SHIFT              61
#define AMDVI_DEV_PERM_READ               (1ULL << 61)
#define AMDVI_DEV_PERM_WRITE              (1ULL << 62)

/* Device table entry bits 64:127 */
#define AMDVI_DEV_DOMID_ID_MASK             GENMASK64(15, 0)

#define AMDVI_FEATURE_PREFETCH            (1ULL << 0) /* page prefetch       */
#define AMDVI_FEATURE_PPR                 (1ULL << 1) /* PPR Support         */
#define AMDVI_FEATURE_XT                  (1ULL << 2) /* x2APIC Support      */
#define AMDVI_FEATURE_GT                  (1ULL << 4) /* Guest Translation   */
#define AMDVI_FEATURE_IA                  (1ULL << 6) /* inval all support   */
#define AMDVI_FEATURE_GA                  (1ULL << 7) /* guest VAPIC support */
#define AMDVI_FEATURE_HE                  (1ULL << 8) /* hardware error regs */
#define AMDVI_FEATURE_PC                  (1ULL << 9) /* Perf counters       */

typedef struct AMDVIAsKey {
    PCIBus *bus;
    uint8_t devfn;
} AMDVIAsKey;

PCIBus *get_root_bus(struct PCIBus *bus);
guint amdvi_as_hash(gconstpointer v);
gboolean amdvi_as_equal(gconstpointer v1, gconstpointer v2);

typedef struct AMDVIState AMDVIState;
void amdvi_set_quad(AMDVIState *s, hwaddr addr, uint64_t val,
                    uint64_t romask, uint64_t w1cmask);
uint16_t amdvi_readw(AMDVIState *s, hwaddr addr);
uint32_t amdvi_readl(AMDVIState *s, hwaddr addr);
uint64_t amdvi_readq(AMDVIState *s, hwaddr addr);
void amdvi_writew(AMDVIState *s, hwaddr addr, uint16_t val);
void amdvi_writel(AMDVIState *s, hwaddr addr, uint32_t val);
void amdvi_writeq(AMDVIState *s, hwaddr addr, uint64_t val);
void amdvi_mmio_reg_write(AMDVIState *s, unsigned size, uint64_t val, hwaddr addr);
