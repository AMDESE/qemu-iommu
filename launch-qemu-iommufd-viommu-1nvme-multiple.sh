#!/bin/bash

QEMU=/home/amd/qemu/build/qemu-system-x86_64

# NVME is
# 42:00.0 Non-Volatile memory controller: Samsung Electronics Co Ltd NVMe SSD Controller PM9A1/PM9A3/980PRO

DEV_IOMMU="0000:40:00.2"

DEV_LIST="\
0000:92:00.0 \
0000:81:00.0 \
"
#########################################
# BINDING

ulimit -c unlimited
sysctl -w kernel.core_pattern=/tmp/core.%e.%p

echo 'file drivers/iommu/amd/iommufd.c +p' >  /sys/kernel/debug/dynamic_debug/control
echo 'file drivers/iommu/amd/viommu.c +p' >  /sys/kernel/debug/dynamic_debug/control
echo 'file drivers/iommu/amd/init.c +p' >  /sys/kernel/debug/dynamic_debug/control
echo 'file drivers/iommu/amd/trans_devid.c +p' >  /sys/kernel/debug/dynamic_debug/control
echo 'file drivers/iommu/amd/vfctrl_mmio.c +p' >  /sys/kernel/debug/dynamic_debug/control

#dmesg -n8
#modprobe -r vfio-pci
#modprobe -r iommufd
#modprobe -r kvm_amd
#
#for i in $DEV_LIST
#do
#       DEVID=`lspci -n -s $i| awk -F '[ :]' '{print $5" "$6}'`
#
#       #-----------------------------------
#       # Unbind the drivers
#       echo "Unbinding ... $i"
#       echo $i> "/sys/bus/pci/devices/$i/driver/unbind"
#done
#
#modprobe kvm_amd avic=1
#modprobe iommufd
#modprobe vfio-pci
#
#for i in $DEV_LIST
#do
#       DEVID=`lspci -n -s $i| awk -F '[ :]' '{print $5" "$6}'`
#
#       # Bind NIC to vfio-pci
#       echo "Binding ... vfio-pci $i"
#       echo $DEVID > /sys/bus/pci/drivers/vfio-pci/new_id
#       echo $DEVID > /sys/bus/pci/drivers/vfio-pci/bind
#done

########################################

#-trace events=events.txt -trace file=trace.log \
ARGS="\
-smp 1 \
-nographic \
-object iommufd,id=iommufd1 \
-enable-kvm -cpu host \
-machine q35,kernel_irqchip=split,memory-backend=ram1 \
-object memory-backend-memfd,id=ram1,size=4G,share=true,reserve=false \
-device virtio-scsi-pci,id=scsi,bus=pcie.0 \
-device scsi-hd,drive=drive0 \
-drive file=/home/amd/disk.img,if=none,id=drive0 \
-device e1000,netdev=user.0 -netdev user,id=user.0,hostfwd=tcp::5555-:22 \
"

ARGS="$ARGS -device pxb-pcie,id=pcie.2,bus=pcie.0,bus_nr=0x20"
ARGS="$ARGS -device pcie-root-port,bus=pcie.2,id=rp1,slot=3,multifunction=on,chassis=3"
ARGS="$ARGS -device AMD-VIOMMU-PCI,id=iommupci0,bus=rp1"
ARGS="$ARGS -device amd-viommu,intremap=off,last-bus-nr=0x2F,pci-id=iommupci0"
ARGS="$ARGS -device pcie-root-port,bus=pcie.2,id=rp2,slot=4,multifunction=on,chassis=4"
ARGS="$ARGS -device vfio-pci,host=0000:92:00.0,iommufd=iommufd1,bus=rp2"
#
ARGS="$ARGS -device pxb-pcie,id=pcie.3,bus=pcie.0,bus_nr=0x70"
ARGS="$ARGS -device pcie-root-port,bus=pcie.3,id=rp4,slot=6,multifunction=on,chassis=3"
ARGS="$ARGS -device AMD-VIOMMU-PCI,id=iommupci1,bus=rp4"
ARGS="$ARGS -device amd-viommu,intremap=off,last-bus-nr=0x7F,pci-id=iommupci1"
ARGS="$ARGS -device pcie-root-port,bus=pcie.3,id=rp5,slot=5,multifunction=on,chassis=4"
ARGS="$ARGS -device vfio-pci,host=0000:81:00.0,iommufd=iommufd1,bus=rp5"

##for j in $DEV_LIST2
##do
##      #ARGS="$ARGS -device vfio-pci,host=$j,iommufd=iommufd1,parent-iommu-id=2,bus=rp3"
##      ARGS="$ARGS -device vfio-pci,host=$j,iommufd=iommufd1,bus=rp3"
##done
#
# Start VM
echo Launching QEMU: $ARGS
#gdb -ex start --args $QEMU $ARGS \
#gdb --args $QEMU $ARGS \
$QEMU $ARGS \
-kernel /home/amd/vmlinuz-7.1.0-rc1-qemu \
-initrd /home/amd/initrd.img-7.1.0-rc1-qemu \
-append 'root=/dev/sda3 ro log_buf_len=10M console=tty0 console=ttyS0,115200n8 cloud-init=disabled amd_iommu_dump=1 no5lvl amd_iommu=pgtbl_v2 intremap=off dyndbg="file drivers/iommu/amd/iommu.c +p"'
