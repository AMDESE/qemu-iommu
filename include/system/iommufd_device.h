/*
 * IOMMUFD device interface (non-QOM)
 *
 * Copyright (C) 2023 Intel Corporation.
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_IOMMUFD_DEVICE_H
#define SYSTEM_IOMMUFD_DEVICE_H

#include <linux/iommufd.h>
#include "system/iommufd.h"

typedef struct IOMMUFDDevice IOMMUFDDevice;

typedef struct IOMMUFDDeviceOps {
    int (*attach_hwpt)(IOMMUFDDevice *idev, uint32_t hwpt_id);
    int (*detach_hwpt)(IOMMUFDDevice *idev);
} IOMMUFDDeviceOps;

/* Abstraction of a device file descriptor opened with iommufd. Not a QOM type. */
struct IOMMUFDDevice {
    IOMMUFDDeviceOps *ops;
    IOMMUFDBackend *iommufd;
    uint32_t dev_id;
    uint32_t def_hwpt_id;
    bool initialized;
};

int iommufd_device_attach_hwpt(IOMMUFDDevice *idev, uint32_t hwpt_id);
int iommufd_device_detach_hwpt(IOMMUFDDevice *idev);
int iommufd_device_get_info(IOMMUFDDevice *idev, uint32_t *type,
                            uint32_t len, void *data);
void iommufd_device_init(void *_idev, size_t instance_size,
                         IOMMUFDDeviceOps *ops, IOMMUFDBackend *iommufd,
                         uint32_t dev_id, uint32_t hwpt_id);
void iommufd_device_destroy(IOMMUFDDevice *idev);

#endif
