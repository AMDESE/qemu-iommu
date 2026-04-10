/*
 * QEMU support of AMD HW-assisted Secure VIOMMU
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_I386_AMD_SVIOMMU_H
#define HW_I386_AMD_SVIOMMU_H

#include "hw/i386/x86-iommu.h"
#include "system/iommufd.h"
#include "amd_iommu_common.h"

#define TYPE_AMD_SVIOMMU_PCI "AMD-SVIOMMU-PCI"
#define AMD_SVIOMMU_PCI(obj)\
    OBJECT_CHECK(AMDVIPCIState, (obj), TYPE_AMD_SVIOMMU_PCI)

#define TYPE_AMD_SVIOMMU_DEVICE "amd-sviommu"
OBJECT_DECLARE_SIMPLE_TYPE(AMDSVIOMMUState, AMD_SVIOMMU_DEVICE)

/* capabilities header */
#define AMD_SVIOMMU_CAPAB_FEATURES (AMDVI_CAPAB_FLAT_EXT | \
        AMDVI_CAPAB_FLAG_IOTLBSUP | \
        AMDVI_CAPAB_ID_SEC | AMDVI_CAPAB_INIT_TYPE | \
        AMDVI_CAPAB_FLAG_HTTUNNEL |  AMDVI_CAPAB_EFR_SUP | \
        AMDVI_CAPAB_FLAG_NPCACHE)

/*
 * In secure vIOMMU configurations only the third 4K page of the MMIO region
 * is accelerated. The 1st 8K and the 4th 4K are guest private memory.
 */
#define AMD_SVIOMMU_MMIO1_BASE      (0x0000)
#define AMD_SVIOMMU_MMIO1_SIZE      (0x2000)
#define AMD_SVIOMMU_VF_MMIO_BASE    (0x2000)
#define AMD_SVIOMMU_VF_MMIO_SIZE    (0x1000)
#define AMD_SVIOMMU_MMIO4_BASE      (0x3000)
#define AMD_SVIOMMU_MMIO4_SIZE      (0x1000)

struct AMDSVIOMMUState {
    X86IOMMUState iommu;        /* IOMMU bus device             */
    AMDVIPCIState *pci;         /* IOMMU PCI device             */
    char *pci_id;               /* ID of AMD-SVIOMMU-PCI device */

    AMDIVRSInfo ivrs_info;

    IOMMUFDBackend *iommufd;                /* IOMMUFD backend object  */
    IOMMUFDViommu viommu;                   /* secure vIOMMU object    */
    GHashTable *iommufd_dev_hash;           /* IOMMUFD device list     */
    struct iommu_viommu_amd iommufd_viommu_amd;
    struct iommu_hw_info_amd *hwinfo;
    bool enabled;

    MemoryRegion mr_mmio;       /* Trapped MMIO region          */
    MemoryRegion mr_mmio1;      /* 1st 8K for Secure HW-vIOMMU  */
    MemoryRegion mr_mmio3;      /* 3rd 4K for Secure HW-vIOMMU  */
    MemoryRegion mr_mmio4;      /* 4th 4K for Secure HW-vIOMMU  */
    void *mmio_page3;           /* VF MMIO mmap pointer         */
};

typedef struct AMDSVIOMMUDevice {
    AMDSVIOMMUState *sviommu;
    HostIOMMUDevice *hiod;
    PCIBus *bus;
    uint8_t devfn;
    uint16_t gdevid;
} AMDSVIOMMUDevice;

#endif /* HW_I386_AMD_SVIOMMU_H */
