#!/bin/bash

QEMU=/home/vasant/git/qemu-sviommu/build/qemu-system-x86_64

DEV_IOMMU="0000:50:00.2"
DEV_LIST="0000:51:00.0"
IMAGE="/home/vasant/images/ubuntu-24.04-server-cloudimg-amd64.qcow2"

#########################################
# BINDING

ulimit -c unlimited
sysctl -w kernel.core_pattern=/tmp/core.%e.%p

dmesg -n8

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
        echo $i > /sys/bus/pci/drivers/vfio-pci/bind
done

########################################

#-trace events=events.txt -trace file=trace.log \
ARGS="\
-smp 1 \
-nographic \
-object iommufd,id=iommufd1 \
-enable-kvm -cpu host \
-machine q35,kernel_irqchip=split,memory-backend=ram1 \
-object memory-backend-memfd,id=ram1,size=4G,share=true,reserve=false \
-drive file=${IMAGE},format=qcow2,if=none,id=drive-virtio-disk4,cache=writethrough \
-device virtio-blk-pci,bus=pcie.0,addr=0x5,drive=drive-virtio-disk4,id=virtio-disk4,bootindex=1 \
-device e1000,netdev=user.0 -netdev user,id=user.0,hostfwd=tcp::5555-:22 \
"


ARGS="$ARGS -device pxb-pcie,id=pcie.2,bus=pcie.0,bus_nr=0x20"
ARGS="$ARGS -device pcie-root-port,bus=pcie.2,id=rp1,slot=3,multifunction=on,chassis=3"
ARGS="$ARGS -device AMD-VIOMMU-PCI,id=iommupci0,bus=rp1"
ARGS="$ARGS -device amd-viommu,intremap=off,pci-id=iommupci0"
ARGS="$ARGS -device pcie-root-port,bus=pcie.2,id=rp2,slot=4,multifunction=on,chassis=4"
ARGS="$ARGS -device vfio-pci,host=0000:51:00.0,iommufd=iommufd1,bus=rp2"

# Start VM
echo Launching QEMU: $ARGS
$QEMU $ARGS \
-kernel /home/vasant/git/linux-guest/arch/x86_64/boot/bzImage \
-initrd  /home/vasant/git/linux-guest/initrd \
-append "root=/dev/vda1 ro log_buf_len=10M console=tty0 console=ttyS0,115200n8 cloud-init=disabled amd_iommu_dump=1 no5lvl amd_iommu=pgtbl_v2 intremap=off "
