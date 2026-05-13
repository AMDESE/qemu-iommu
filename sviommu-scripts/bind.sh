#! /bin/bash

#
#2) Bind script - example execution: ./bind "0000:e1:04.0"
#

DEVID_KSB0_VFS=("0000:61:04.0" "0000:61:04.1" "0000:61:04.2" "0000:61:04.3")

modprobe vfio_pci

for DEV in "${DEVID_KSB0_VFS[@]}"; do
	echo "$DEV" > "/sys/bus/pci/devices/$DEV/driver/unbind"
	echo vfio-pci > "/sys/bus/pci/devices/$DEV/driver_override"
	echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/bind
	echo '' > "/sys/bus/pci/devices/$DEV/driver_override"

	sleep 1
done
#chown amd:amd /dev/vfio/* /dev/vfio/devices/vfio* /dev/iommu
