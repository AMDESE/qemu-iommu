#!/bin/bash
#
ulimit -c unlimited
QEMU=/home/vasant/git/qemu-sviommu/build/qemu-system-x86_64

OVMF=/home/vasant/images/0407/OVMF.fd

DISK=/home/vasant/images/0407/u2204_128G_tsm_0407.qcow2

DEV_IOMMU1="0000:60:00.2"

KSB_VF="0000:61:04.0"

${QEMU} \
-enable-kvm \
-smp 2 \
-machine q35,confidential-guest-support=sev0,memory-backend=ram1 \
-device virtio-scsi-pci,id=vscsi0,iommu_platform=on,disable-modern=off,disable-legacy=on \
-nographic \
-vga none \
-chardev stdio,id=STDIO0,signal=off,mux=on \
-device isa-serial,id=isa-serial0,chardev=STDIO0 \
-mon id=MON0,chardev=STDIO0,mode=readline \
-bios ${OVMF} \
-chardev socket,id=SOCKET0,server=on,wait=off,path=qemu.mon.q.tvm3 \
-mon chardev=SOCKET0,mode=readline \
-netdev user,id=USER0,hostfwd=tcp::3333-:22 \
-device virtio-net-pci,id=vnet0,iommu_platform=on,disable-legacy=on,romfile=,netdev=USER0 \
-cpu EPYC-Milan-v2,phys-bits=52 \
-object sev-snp-guest,id=sev0,cbitpos=51,reduced-phys-bits=1,id-auth=,policy=0x30000,kernel-hashes=off,convert-in-place=true,gmem-allocator=hugetlb,gmem-page-size=2097152 \
-object memory-backend-memfd,id=ram1,size=4G,share=true,reserve=on,prealloc=on \
-object iommufd,id=i0 \
-device pcie-root-port,id=r0,slot=0,addr=6 \
-device vfio-pci,id=vfio0000_c1_04_2,host=${KSB_VF},bus=r0,iommufd=i0 \
-drive id=DRIVE0,if=none,file=${DISK},format=qcow2 \
-device scsi-hd,id=scsi-hd0,drive=DRIVE0 \
-snapshot \
-trace events=qemu_trace_events \
-d guest_errors \
-chardev socket,id=SOCKET1,server=on,wait=off,path=qemu.mon.user3333 \
-mon chardev=SOCKET1,mode=control \
-name user3333,debug-threads=on \
-kernel /home/vasant/git/linux-guest/arch/x86_64/boot/bzImage \
-initrd /home/vasant/git/linux-guest/initrd \
-append "root=UUID=2e220180-8789-487a-8016-52fdeb59554b ro debug iommu=pt amd_iommu_dump=1 loglevel=8 console=ttyS0 amd_iommu=pgtbl_v2 earlyprintk accept_memory=eager "
