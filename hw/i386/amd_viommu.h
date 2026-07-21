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
    GHashTable *iommufd_dev_hash;           /* IOMMUFD device list          */
} AMDIOMMUFDState;

struct AMDVIOMMUState {
    X86IOMMUState iommu;        /* IOMMU bus device             */
    AMDIOMMUFDState iommufd_state;
    AMDVIPCIState *pci;         /* IOMMU PCI device             */
    char *pci_id;               /* ID of AMDVI-PCI device, if user created */

    AMDIVRSInfo ivrs_info;

    char *phys_iommu_path;
};

typedef struct AMDIOMMUFDDevice {
    AMDVIOMMUState *viommu;
    HostIOMMUDevice *hiod;
} AMDIOMMUFDDevice;
