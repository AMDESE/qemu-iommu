
#ifndef HW_I386_ACPI_BUILD_H
#define HW_I386_ACPI_BUILD_H
#include "hw/acpi/acpi-defs.h"
#include "qemu/bitops.h"

extern const struct AcpiGenericAddress x86_nvdimm_acpi_dsmio;

void acpi_setup(void);
Object *acpi_get_i386_pci_host(void);

#define AMD_IVINFO_EFR_SUP       BIT(0)

#define AMD_IVINFO_PASIZE_SHIFT  (8)
#define AMD_IVINFO_PASIZE_52BIT  (0x34 << AMD_IVINFO_PASIZE_SHIFT)

#define AMD_IVHD_FLAG_HT_TUN_EN    BIT(0)
#define AMD_IVHD_FLAG_IOTLB_SUP    BIT(4)
#define AMD_IVHD_FLAG_PREF_SUP     BIT(6)
#define AMD_IVHD_FLAG_PPR_SUP      BIT(7)

#define AMD_IVHD_FEATURE_REPORT_XT_SUP_SHIFT      (0)
#define AMD_IVHD_FEATURE_REPORT_GT_SUP_SHIFT      (2)
#define AMD_IVHD_FEATURE_REPORT_GA_SUP_SHIFT      (6)
#define AMD_IVHD_FEATURE_REPORT_GATS_SHIFT        (28)
#define AMD_IVHD_FEATURE_REPORT_HATS_SHIFT        (30)

#define AMD_IVHD_ATTRIBUTES_HATDIS_SHIFT        (0)

#define AMD_IVHD_DEVICE_ENTRY_TYPE_RESERVED             (0)
#define AMD_IVHD_DEVICE_ENTRY_TYPE_ALL                  (1)
#define AMD_IVHD_DEVICE_ENTRY_TYPE_SELECT               (2)
#define AMD_IVHD_DEVICE_ENTRY_TYPE_START_RANGE          (3)
#define AMD_IVHD_DEVICE_ENTRY_TYPE_END_RANGE            (4)
#define AMD_IVHD_DEVICE_ENTRY_TYPE_ALIAS_START_RANGE    (0x43)
#define AMD_IVHD_DEVICE_ENTRY_TYPE_SPECIAL_DEVICE       (0x48)

#define IVHD_VARIETY_IOAPIC (1)
#define IVHD_VARIETY_HPET   (2)

/* Vendor(AMD) specific fields in the IVRS header
 * Excludes fields in ACPI table header
 */
typedef
struct AmdIvrsVendorHdr {
    uint32_t ivinfo;
    uint64_t reserved;
} __attribute__((packed)) AmdIvrsVendorHdr;

/* IVHD type 10h */
typedef
struct AmdIvhdHdr10 {
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint16_t devid;
    uint16_t capab_offset;
    uint64_t base_addr;
    uint16_t pci_seg;
    uint16_t iommu_info;
    uint32_t iommu_feature_report;
} __attribute__((packed)) AmdIvhdHdr10;

/* IVHD type 11h */
typedef
struct AmdIvhdHdr11 {
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint16_t devid;
    uint16_t capab_offset;
    uint64_t base_addr;
    uint16_t pci_seg;
    uint16_t iommu_info;
    uint32_t iommu_attributes;
    uint64_t efr;
    uint64_t efr2;
} __attribute__((packed)) AmdIvhdHdr11;

typedef
struct AmdIvhdDeviceEntry {
    uint8_t type;
    uint16_t devid;
    uint8_t dte_setting;
} __attribute__((packed)) AmdIvhdDeviceEntry;

typedef
struct AmdIvhdDeviceEntryExt {
    uint8_t type;
    uint16_t devid_a;
    uint8_t dte_setting;
    union {
        struct {
            uint8_t handle;
            uint16_t devid_b;
            uint8_t varity;
        } __attribute__((packed));
        struct {
            uint32_t ext_dte_setting;
        } __attribute__((packed));
    };
} __attribute__((packed)) AmdIvhdDeviceEntryExt;

#endif
