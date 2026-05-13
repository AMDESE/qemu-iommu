#! /bin/bash

#3) Run SNVM-VM == TVM (adjust path to QEMU, qcow2 image and, OVMF.fd.):
#
# hw-viommu QEMU exposes the vIOMMU as a PCI function plus a sysbus
# device (see viommu-launch-script.sh). amd-sviommu takes pci-id= and
# no longer accepts host=, iommu-id=, iommufd=, primary-bus= or
# last-bus-nr=. The TIO VF must be on the same pxb-pcie root as
# AMD-SVIOMMU-PCI. 0000:e1:04.1 is behind host IOMMU 0000:e0:00.2.

ulimit -c unlimited
modprobe kvm_amd

QEMU=/home/vasant/git/qemu-sviommu/build/qemu-system-x86_64

OVMF=/home/vasant/images/0407/OVMF.fd

DISK=/home/vasant/images/0407/u2204_128G_tsm_0407.qcow2

KSB_VF="0000:e1:04.2"

# Previous runs leave these sockets behind and the next launch fails
# to bind them.
rm -f qemu.mon.user3333 qemu.mon.q.tvm3

$QEMU \
	--trace kvm_sev* \
	--trace kvm_set_user_* \
	--trace iommufd*\
	--trace kvm_memory_fault* \
	--trace sev_tio* \
	-enable-kvm \
	-smp 1 \
	-nographic \
	-vga none \
	-mon chardev=SOCKET0,mode=readline \
	-bios ${OVMF} \
	-cpu EPYC-Milan-v2,phys-bits=52 \
	-object iommufd,id=iommufd0 \
	-netdev user,id=USER0,hostfwd=tcp::3333-:22 \
	-device virtio-net-pci,id=vnet0,iommu_platform=on,disable-legacy=on,romfile=,netdev=USER0 \
	-machine q35,confidential-guest-support=sev0,memory-backend=ram1 \
	-device virtio-scsi-pci,id=vscsi0,iommu_platform=on,disable-modern=off,disable-legacy=on \
	-chardev stdio,id=STDIO0,signal=off,mux=on \
	-device isa-serial,id=isa-serial0,chardev=STDIO0 \
	-mon id=MON0,chardev=STDIO0,mode=readline \
	-chardev socket,id=SOCKET1,server=on,wait=off,path=qemu.mon.user3333 \
	-name user3333,debug-threads=on \
	-mon chardev=SOCKET1,mode=control \
	-d guest_errors \
	-chardev socket,id=SOCKET0,server=on,wait=off,path=qemu.mon.q.tvm3 \
	-object sev-snp-guest,id=sev0,cbitpos=51,reduced-phys-bits=1,id-auth=,policy=0x30000,kernel-hashes=off,convert-in-place=true,gmem-allocator=hugetlb,gmem-page-size=2097152 \
	-object memory-backend-memfd,id=ram1,size=4G,share=true,reserve=on,prealloc=on \
	-drive id=DRIVE0,if=none,file=${DISK},format=qcow2 \
	-device scsi-hd,id=scsi-hd0,drive=DRIVE0 \
	-device pxb-pcie,id=pcie.1,bus=pcie.0,bus_nr=0x10 \
	-device pcie-root-port,bus=pcie.1,id=rp1,slot=1,multifunction=on,chassis=1 \
	-device AMD-SVIOMMU-PCI,id=sviommupci0,bus=rp1 \
	-device amd-sviommu,intremap=off,pci-id=sviommupci0 \
	-device pcie-root-port,bus=pcie.1,id=rp2,slot=2,multifunction=on,chassis=2 \
	-device vfio-pci,id=vfio_e1_04_1,host=${KSB_VF},bus=rp2,iommufd=iommufd0,x-tio=true \
	-kernel /home/vasant/git/linux-guest/arch/x86_64/boot/bzImage \
	-initrd /home/vasant/git/linux-guest/initrd \
	-append "root=UUID=2e220180-8789-487a-8016-52fdeb59554b ro debug iommu=nopt amd_iommu_dump=1 loglevel=8 console=ttyS0 amd_iommu=pgtbl_v2 earlyprintk accept_memory=eager "
