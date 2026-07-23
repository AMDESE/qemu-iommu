/*
 * QEMU emulation of an AMD IOMMU (AMD-Vi)
 *
 * Copyright (C) 2011 Eduard - Gabriel Munteanu
 * Copyright (C) 2015, 2016 David Kiarie Kahurani
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AMD_IOMMU_H
#define AMD_IOMMU_H

#include "hw/pci/pci.h"
#include "qom/object.h"
#include "hw/i386/amd_iommu_common.h"

#define AMDVI_CMDBUF_ID_BYTE              0x07
#define AMDVI_CMDBUF_ID_RSHIFT            4

#define AMDVI_CMD_COMPLETION_WAIT         0x01
#define AMDVI_CMD_INVAL_DEVTAB_ENTRY      0x02
#define AMDVI_CMD_INVAL_AMDVI_PAGES       0x03
#define AMDVI_CMD_INVAL_IOTLB_PAGES       0x04
#define AMDVI_CMD_INVAL_INTR_TABLE        0x05
#define AMDVI_CMD_PREFETCH_AMDVI_PAGES    0x06
#define AMDVI_CMD_COMPLETE_PPR_REQUEST    0x07
#define AMDVI_CMD_INVAL_AMDVI_ALL         0x08


#define AMDVI_CMD_INVAL_IOMMU_PAGES_S   (1ULL << 0)
#define AMDVI_INV_ALL_PAGES             (1ULL << 52)

#define AMDVI_DEVTAB_ENTRY_SIZE           32

/* Event codes and flags, as stored in the info field */
#define AMDVI_EVENT_ILLEGAL_DEVTAB_ENTRY  (0x1U << 12)
#define AMDVI_EVENT_IOPF                  (0x2U << 12)
#define   AMDVI_EVENT_IOPF_I              (1U << 3)
#define AMDVI_EVENT_DEV_TAB_HW_ERROR      (0x3U << 12)
#define AMDVI_EVENT_PAGE_TAB_HW_ERROR     (0x4U << 12)
#define AMDVI_EVENT_ILLEGAL_COMMAND_ERROR (0x5U << 12)
#define AMDVI_EVENT_COMMAND_HW_ERROR      (0x6U << 12)

#define AMDVI_EVENT_LEN                  16
#define AMDVI_PERM_READ             (1 << 0)
#define AMDVI_PERM_WRITE            (1 << 1)

/* reserved DTE bits */
#define AMDVI_DTE_QUAD0_RESERVED        (GENMASK64(6, 2) | GENMASK64(63, 63))
#define AMDVI_DTE_QUAD1_RESERVED        0
#define AMDVI_DTE_QUAD2_RESERVED        GENMASK64(53, 52)
#define AMDVI_DTE_QUAD3_RESERVED        (GENMASK64(14, 0) | GENMASK64(53, 48))

/* AMDVI paging mode */
#define AMDVI_GATS_MODE_SHIFT           (12)
#define AMDVI_GATS_MODE_MASK            (3ULL <<  12)
#define AMDVI_GATS_MODE                 (2ULL <<  12)
#define AMDVI_HATS_MODE_SHIFT           (10)
#define AMDVI_HATS_MODE_MASK            (3ULL <<  10)
#define AMDVI_HATS_MODE                 (2ULL <<  10)
#define AMDVI_HATS_MODE_RESERVED        (3ULL <<  10)

/* Page Table format */

#define AMDVI_PTE_PR                    (1ULL << 0)
#define AMDVI_PTE_NEXT_LEVEL_MASK       GENMASK64(11, 9)

#define IOMMU_PTE_PRESENT(pte)          ((pte) & AMDVI_PTE_PR)

/* Using level=1 for leaf PTE at 4K page size */
#define PT_LEVEL_SHIFT(level)           (12 + (((level) - 1) * 9))

/* Return IOVA bit group used to index the Page Table at specific level */
#define PT_LEVEL_INDEX(level, iova)     (((iova) >> PT_LEVEL_SHIFT(level)) & \
                                        GENMASK64(8, 0))

/* Return the maximum output address for a specified page table level */
#define PT_LEVEL_MAX_ADDR(level)    (((level) > 5) ? (~(0ULL)) : \
                                    ((1ULL << PT_LEVEL_SHIFT((level) + 1)) - 1))

/* Extract the NextLevel field from PTE/PDE */
#define PTE_NEXT_LEVEL(pte)     (((pte) & AMDVI_PTE_NEXT_LEVEL_MASK) >> 9)

/* Take page table level and return default pagetable size for level */
#define PTE_LEVEL_PAGE_SIZE(level)      (1ULL << (PT_LEVEL_SHIFT(level)))

/*
 * Return address of lower level page table encoded in PTE and specified by
 * current level and corresponding IOVA bit group at such level.
 */
#define NEXT_PTE_ADDR(pte, level, iova) (((pte) & AMDVI_DEV_PT_ROOT_MASK) + \
                                        (PT_LEVEL_INDEX(level, iova) * 8))

/*
 * Take a PTE value with mode=0x07 and return the page size it encodes.
 */
#define PTE_LARGE_PAGE_SIZE(pte)    (1ULL << (1 + cto64(((pte) | 0xfffULL))))

/* Return number of PTEs to use for a given page size (expected power of 2) */
#define PAGE_SIZE_PTE_COUNT(pgsz)       (1ULL << ((ctz64(pgsz) - 12) % 9))

/* IOTLB */
#define AMDVI_IOTLB_MAX_SIZE        1024
#define AMDVI_GET_IOTLB_GFN(addr)   (addr >> AMDVI_PAGE_SHIFT_4K)

/* default extended feature */
#define AMDVI_DEFAULT_EXT_FEATURES \
        (AMDVI_FEATURE_PREFETCH | AMDVI_FEATURE_PPR | \
        AMDVI_FEATURE_IA | AMDVI_FEATURE_GT | AMDVI_FEATURE_HE | \
        AMDVI_GATS_MODE | AMDVI_HATS_MODE | AMDVI_FEATURE_GA)

/* capabilities header */
#define AMDVI_CAPAB_FEATURES (AMDVI_CAPAB_FLAT_EXT | \
        AMDVI_CAPAB_FLAG_NPCACHE | AMDVI_CAPAB_FLAG_IOTLBSUP \
        | AMDVI_CAPAB_ID_SEC | AMDVI_CAPAB_INIT_TYPE | \
        AMDVI_CAPAB_FLAG_HTTUNNEL |  AMDVI_CAPAB_EFR_SUP)

#define AMDVI_PAGE_SHIFT_4K 12
#define AMDVI_PAGE_MASK_4K      GENMASK64(63, 12)


/* Completion Wait data size */
#define AMDVI_COMPLETION_DATA_SIZE    8

#define AMDVI_COMMAND_SIZE   16

#define AMDVI_INT_ADDR_FIRST    0xfee00000
#define AMDVI_INT_ADDR_LAST     0xfeefffff
#define AMDVI_INT_ADDR_SIZE     (AMDVI_INT_ADDR_LAST - AMDVI_INT_ADDR_FIRST + 1)

/* SB IOAPIC is always on this device in AMD systems */
#define AMDVI_IOAPIC_SB_DEVID   PCI_BUILD_BDF(0, PCI_DEVFN(0x14, 0))

/* Interrupt remapping errors */
#define AMDVI_IR_ERR            0x1
#define AMDVI_IR_GET_IRTE       0x2
#define AMDVI_IR_TARGET_ABORT   0x3

/* Interrupt remapping */
#define AMDVI_IR_REMAP_ENABLE           1ULL
#define AMDVI_IR_INTCTL_SHIFT           60
#define AMDVI_IR_INTCTL_ABORT           0
#define AMDVI_IR_INTCTL_PASS            1
#define AMDVI_IR_INTCTL_REMAP           2

#define AMDVI_IR_PHYS_ADDR_MASK         GENMASK64(51, 6)

/* MSI data 10:0 bits (section 2.2.5.1 Fig 14) */
#define AMDVI_IRTE_OFFSET               0x7ff

/* Delivery mode of MSI data (same as IOAPIC deilver mode encoding) */
#define AMDVI_IOAPIC_INT_TYPE_FIXED          0x0
#define AMDVI_IOAPIC_INT_TYPE_ARBITRATED     0x1
#define AMDVI_IOAPIC_INT_TYPE_SMI            0x2
#define AMDVI_IOAPIC_INT_TYPE_NMI            0x4
#define AMDVI_IOAPIC_INT_TYPE_INIT           0x5
#define AMDVI_IOAPIC_INT_TYPE_EINT           0x7

/* Pass through interrupt */
#define AMDVI_DEV_INT_PASS_MASK         (1ULL << 56)
#define AMDVI_DEV_EINT_PASS_MASK        (1ULL << 57)
#define AMDVI_DEV_NMI_PASS_MASK         (1ULL << 58)
#define AMDVI_DEV_LINT0_PASS_MASK       (1ULL << 62)
#define AMDVI_DEV_LINT1_PASS_MASK       (1ULL << 63)

#define TYPE_AMD_IOMMU_DEVICE "amd-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(AMDVIState, AMD_IOMMU_DEVICE)

#define TYPE_AMD_IOMMU_PCI "AMDVI-PCI"
OBJECT_DECLARE_SIMPLE_TYPE(AMDVIPCIState, AMD_IOMMU_PCI)

#define TYPE_AMD_IOMMU_MEMORY_REGION "amd-iommu-iommu-memory-region"

typedef struct AMDVIAddressSpace AMDVIAddressSpace;

struct AMDVIState {
    X86IOMMUState iommu;        /* IOMMU bus device             */
    AMDVIPCIState *pci;         /* IOMMU PCI device             */
    char *pci_id;               /* ID of AMDVI-PCI device, if user created */

    uint32_t version;

    uint64_t mmio_addr;

    bool enabled;                /* IOMMU enabled                */
    bool cmdbuf_enabled;         /* command buffer enabled       */
    bool evtlog_enabled;         /* event log enabled            */
    bool excl_enabled;

    hwaddr devtab;               /* base address device table    */
    uint64_t devtab_len;         /* device table length          */

    hwaddr cmdbuf;               /* command buffer base address  */
    uint64_t cmdbuf_len;         /* command buffer length        */
    uint32_t cmdbuf_head;        /* current IOMMU read position  */
    uint32_t cmdbuf_tail;        /* next Software write position */
    bool completion_wait_intr;

    hwaddr evtlog;               /* base address event log       */
    bool evtlog_intr;
    uint32_t evtlog_len;         /* event log length             */
    uint32_t evtlog_head;        /* current IOMMU write position */
    uint32_t evtlog_tail;        /* current Software read position */

    /* unused for now */
    hwaddr excl_base;            /* base DVA - IOMMU exclusion range */
    hwaddr excl_limit;           /* limit of IOMMU exclusion range   */
    bool excl_allow;             /* translate accesses to the exclusion range */
    bool excl_enable;            /* exclusion range enabled          */

    hwaddr ppr_log;              /* base address ppr log */
    uint32_t pprlog_len;         /* ppr log len  */
    uint32_t pprlog_head;        /* ppr log head */
    uint32_t pprlog_tail;        /* ppr log tail */

    MemoryRegion mr_mmio;              /* MMIO region                  */
    MemoryRegion mr_sys;
    MemoryRegion mr_nodma;
    MemoryRegion mr_ir;
    uint8_t mmior[AMDVI_MMIO_SIZE];    /* read/write MMIO              */
    uint8_t w1cmask[AMDVI_MMIO_SIZE];  /* read/write 1 clear mask      */
    uint8_t romask[AMDVI_MMIO_SIZE];   /* MMIO read/only mask          */
    bool mmio_enabled;

    /* for each served device */
    GHashTable *address_spaces;

    /* list of address spaces with registered notifiers */
    QLIST_HEAD(, AMDVIAddressSpace) amdvi_as_with_notifiers;

    /* IOTLB */
    GHashTable *iotlb;

    /* Interrupt remapping */
    bool ga_enabled;
    bool xtsup;     /* xtsup=on command line */
    bool xten;      /* guest controlled, x2apic mode enabled */
    bool intcapxten; /* guest controlled, IOMMU x2apic interrupts enabled */

    /* DMA address translation */
    bool dma_remap;

   AMDIVRSInfo ivrs_info;
};

#endif
