#include "hw/i386/x86-iommu.h"
#include "system/iommufd.h"

#define TYPE_AMD_VIOMMU_PCI "AMD-VIOMMU-PCI"
#define AMD_VIOMMU_PCI(obj)\
    OBJECT_CHECK(AMDVIPCIState, (obj), TYPE_AMD_VIOMMU_PCI)

/* capabilities header */
#define AMD_VIOMMU_CAPAB_FEATURES (AMDVI_CAPAB_FLAT_EXT | \
        AMDVI_CAPAB_FLAG_IOTLBSUP | \
        AMDVI_CAPAB_ID_SEC | AMDVI_CAPAB_INIT_TYPE | \
        AMDVI_CAPAB_FLAG_HTTUNNEL |  AMDVI_CAPAB_EFR_SUP)


#define TYPE_AMD_VIOMMU_DEVICE "amd-viommu"
OBJECT_DECLARE_SIMPLE_TYPE(AMDVIOMMUState, AMD_VIOMMU_DEVICE)

typedef struct AMDIOMMUFDState {
    IOMMUFDBackend *iommufd;                /* IOMMUFD backend object       */
    uint32_t viommu_id;                     /* virtual IOMMU ID             */
    GHashTable *iommufd_dev_hash;           /* IOMMUFD device list          */
    struct iommu_viommu_amd iommufd_viommu_amd;
    uint32_t parent_hwpt_id;                /* HWPT ID of the parent domain */
    uint32_t devid;                         /* IOMMUFD device ID            */
    bool enable_iommu;
} AMDIOMMUFDState;

struct AMDVIOMMUState {
    X86IOMMUState iommu;        /* IOMMU bus device             */
    AMDIOMMUFDState iommufd_state;
    AMDVIPCIState *pci;         /* IOMMU PCI device             */
    char *pci_id;               /* ID of AMDVI-PCI device, if user created */

    AMDIVRSInfo ivrs_info;

    char *phys_iommu_path;

    struct iommu_hw_info_amd *hwinfo;

    MemoryRegion mr_vf_mmio;            /* Accelerated MMIO memory region */
    void *vf_mmio_page;

    MemoryRegion mr_mmio;              /* MMIO region                  */
    uint8_t mmior[AMDVI_MMIO_SIZE];    /* read/write MMIO              */
    uint8_t w1cmask[AMDVI_MMIO_SIZE];  /* read/write 1 clear mask      */
    uint8_t romask[AMDVI_MMIO_SIZE];   /* MMIO read/only mask          */
};

typedef struct AMDIOMMUFDDevice {
    AMDVIOMMUState *viommu;
    HostIOMMUDevice *hiod;
    uint32_t vdevice_id;
} AMDIOMMUFDDevice;

#define AMD_VIOMMU_SUPPORTED_EFR \
                        (AMDVI_FEATURE_PPR | AMDVI_FEATURE_XT | \
                         AMDVI_FEATURE_GT | AMDVI_FEATURE_GA | \
                         AMDVI_FEATURE_GIO | AMDVI_FEATURE_EPHSUP | \
                         AMDVI_FEATURE_GATS_5LEVEL | AMDVI_FEATURE_GLX_2LEVEL | \
                         AMDVI_FEATURE_PASMAX_16)

#define AMD_VIOMMU_VF_MMIO_BASE     (0x2000)
#define AMD_VIOMMU_VF_MMIO_SIZE     (0x1000)

#define DEFINE_AMD_VIOMMU_READ(name, TYPE, LD)                                \
    DEFINE_AMD_IOMMU_READ(amd_viommu_read, name, AMDVIOMMUState, TYPE, LD)

#define DEFINE_AMD_VIOMMU_WRITE(name, TYPE, LD, ST)                           \
    DEFINE_AMD_IOMMU_WRITE(amd_viommu_write, name, AMDVIOMMUState, TYPE, LD, ST)
