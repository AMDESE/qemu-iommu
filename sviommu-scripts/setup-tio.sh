#! /bin/bash

LINUX_SRC="/home/vasant/git/linux"

PCIBUS=0000:61

sudo modprobe kvm vm_memory_attributes=false
#sudo modprobe ccp
sudo modprobe ccp tiolog=123
sudo bash -c 'echo "module ccp -p" > /sys/kernel/debug/dynamic_debug/control'
sudo modprobe tmpm
sudo modprobe kvm_amd
sudo dmesg -n 8
sudo bash -c "echo 0 > /sys/module/doe/parameters/delay"
#sudo modprobe tsm # Do not normally need this unless rmmod"d
sudo modprobe ksb_pci_drv


echo 'file drivers/iommu/amd/iommu.c func amd_iommu_domain_alloc_paging_flags +p' > /sys/kernel/debug/dynamic_debug/control

for i in 0 1 ; do
        sudo bash -c "echo 0 > /sys/bus/pci/devices/$PCIBUS:00.$i/sriov_numvfs"
        sudo bash -c "echo 4 > /sys/bus/pci/devices/$PCIBUS:00.$i/sriov_numvfs"
done

sudo bash -c "echo \"RUN: Selective stream 0\" > /dev/kmsg"
sudo bash -c "echo tsm0 > /sys/bus/pci/devices/$PCIBUS:00.0/tsm/connect"
sudo bash -c "echo \"RUN: DEV_CONNECT done\" > /dev/kmsg"
cat "/sys/bus/pci/devices/$PCIBUS:00.0/tsm/connect"

sudo bash -c "echo 6000 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"

${LINUX_SRC}/tools/crypto/tsm/tsmsysfs.py --dev /sys/bus/pci/devices/$PCIBUS:00.0/tsm/dev_status
${LINUX_SRC}/tools/crypto/tsm/ide.sh $PCIBUS:00.0
