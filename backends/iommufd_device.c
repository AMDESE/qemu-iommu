/*
 * IOMMUFDDevice helpers
 *
 * Copyright (C) 2023 Intel Corporation.
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"
#include "system/iommufd_device.h"

int iommufd_device_attach_hwpt(IOMMUFDDevice *idev, uint32_t hwpt_id)
{
    g_assert(idev->ops->attach_hwpt);
    return idev->ops->attach_hwpt(idev, hwpt_id);
}

int iommufd_device_detach_hwpt(IOMMUFDDevice *idev)
{
    g_assert(idev->ops->detach_hwpt);
    return idev->ops->detach_hwpt(idev);
}

int iommufd_device_get_info(IOMMUFDDevice *idev, uint32_t *type,
                            uint32_t len, void *data)
{
    struct iommu_hw_info info = {
        .size = sizeof(info),
        .flags = 0,
        .dev_id = idev->dev_id,
        .data_len = len,
        .data_uptr = (uintptr_t)data,
    };
    int ret;

    ret = ioctl(idev->iommufd->fd, IOMMU_GET_HW_INFO, &info);
    if (ret) {
        error_report("Failed to get IOMMU hardware info: %s", strerror(errno));
    } else if (type) {
        *type = info.out_data_type;
    }

    return ret;
}

void iommufd_device_init(void *_idev, size_t instance_size,
                         IOMMUFDDeviceOps *ops, IOMMUFDBackend *iommufd,
                         uint32_t dev_id, uint32_t hwpt_id)
{
    IOMMUFDDevice *idev = (IOMMUFDDevice *)_idev;

    g_assert(sizeof(IOMMUFDDevice) <= instance_size);

    idev->ops = ops;
    idev->iommufd = iommufd;
    idev->dev_id = dev_id;
    idev->def_hwpt_id = hwpt_id;
    idev->initialized = true;
}

void iommufd_device_destroy(IOMMUFDDevice *idev)
{
    if (!idev->initialized) {
        return;
    }
    idev->initialized = false;
    idev->ops = NULL;
}
