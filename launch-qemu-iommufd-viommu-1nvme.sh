#!/bin/bash

QEMU=/sandbox/viommu/qemu-nicolinc_iommufd_iommufd_hw_queue-v8-build/bin/qemu-system-x86_64

# NVME is
# 42:00.0 Non-Volatile memory controller: Samsung Electronics Co Ltd NVMe SSD Controller PM9A1/PM9A3/980PRO

DEV_IOMMU="0000:40:00.2"

DEV_LIST="\
0000:42:00.0 \
"
#########################################
# BINDING

ulimit -c unlimited
sysctl -w kernel.core_pattern=/tmp/core.%e.%p

dmesg -n8
modprobe -r vfio-pci 
modprobe -r iommufd
modprobe -r kvm_amd

for i in $DEV_LIST
do 
	DEVID=`lspci -n -s $i| awk -F '[ :]' '{print $5" "$6}'`

	#-----------------------------------
	# Unbind the drivers
	echo "Unbinding ... $i"
	echo $i> "/sys/bus/pci/devices/$i/driver/unbind"
done

modprobe kvm_amd avic=1
modprobe iommufd
modprobe vfio-pci

for i in $DEV_LIST
do 
	DEVID=`lspci -n -s $i| awk -F '[ :]' '{print $5" "$6}'`

	# Bind NIC to vfio-pci
	echo "Binding ... vfio-pci $i"
	echo $DEVID > /sys/bus/pci/drivers/vfio-pci/new_id
done

########################################

ARGS="\
-trace events=events.txt -trace file=trace.log \
-smp 1 \
-nographic \
-object iommufd,id=iommufd1 \
-enable-kvm -cpu host \
-machine q35,kernel_irqchip=split,memory-backend=ram1 \
-object memory-backend-memfd,id=ram1,size=4G,share=true,reserve=false \
-device virtio-scsi-pci,id=scsi,bus=pcie.0 \
-device scsi-hd,drive=drive0 \
-drive file=/sandbox/vm-images/ubuntu-18.04-100G-viommu-sdxi.qcow2,if=none,id=drive0 \
-device e1000,netdev=user.0 -netdev user,id=user.0,hostfwd=tcp::5555-:22 \
"

ARGS="$ARGS -device pxb-pcie,id=pcie.2,bus=pcie.0,bus_nr=0x20,parent-iommu-id1=2"
ARGS="$ARGS -device pcie-root-port,bus=pcie.2,id=rp3,slot=3,multifunction=on,chassis=3"
ARGS="$ARGS -device amd-viommu,host=$DEV_IOMMU,iommu-id=2,intremap=off,id=amd_viommu_1,iommufd=iommufd1,translate-id=0x4800,primary-bus=pcie.2,last-bus-nr=0x2F"
for j in $DEV_LIST2
do
	#ARGS="$ARGS -device vfio-pci,host=$j,iommufd=iommufd1,parent-iommu-id=2,bus=rp3"
	ARGS="$ARGS -device vfio-pci,host=$j,iommufd=iommufd1,bus=rp3"
done

# Start VM
echo Launching QEMU: $ARGS
#gdb -ex start --args $QEMU $ARGS \
#gdb --args $QEMU $ARGS \
$QEMU $ARGS \
-kernel /boot/vmlinuz-6.12.0-debugfs+ \
-initrd /boot/initrd.img-6.12.0-debugfs+ \
-append "root=UUID=340c0976-2ed4-11eb-91f7-52540012cd16 ro log_buf_len=10M console=tty0 console=ttyS0,115200n8 cloud-init=disabled modprobe.blacklist=mlx5_core,e1000e,nvme,ixgbe,bnxt_en amd_iommu_dump=1 no5lvl amd_iommu=pgtbl_v2 intremap=off "
