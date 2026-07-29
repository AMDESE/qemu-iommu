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
    uint32_t parent_hwpt_id;                /* HWPT ID of the parent domain */
    uint32_t devid;                         /* IOMMUFD device ID            */
} AMDIOMMUFDState;

struct AMDVIOMMUState {
    X86IOMMUState iommu;        /* IOMMU bus device             */
    AMDIOMMUFDState iommufd_state;
    AMDVIPCIState *pci;         /* IOMMU PCI device             */
    char *pci_id;               /* ID of AMDVI-PCI device, if user created */

    AMDIVRSInfo ivrs_info;

    char *phys_iommu_path;

    struct iommu_hw_info_amd *hwinfo; /* HWinfo provided by device */
};

typedef struct AMDIOMMUFDDevice {
    AMDVIOMMUState *viommu;
    HostIOMMUDevice *hiod;
} AMDIOMMUFDDevice;

 #define AMD_VIOMMU_SUPPORTED_EFR \
                        (AMDVI_FEATURE_PPR | AMDVI_FEATURE_XT | \
                         AMDVI_FEATURE_GT | AMDVI_FEATURE_GA | \
                         AMDVI_FEATURE_GIO | AMDVI_FEATURE_EPHSUP | \
                         AMDVI_FEATURE_GATS_5LEVEL | AMDVI_FEATURE_GLX_2LEVEL | \
                         AMDVI_FEATURE_PASMAX_16)
