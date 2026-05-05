#define TYPE_AMD_VIOMMU_PCI "AMD-VIOMMU-PCI"
#define AMD_VIOMMU_DEVICE(obj)\
    OBJECT_CHECK(AMDVIState, (obj), TYPE_AMD_VIOMMU_DEVICE)

#define TYPE_AMD_VIOMMU_DEVICE "amd-viommu"
#define TYPE_AMD_VIOMMU_MEMORY_REGION "amd-viommu-memory-region"
