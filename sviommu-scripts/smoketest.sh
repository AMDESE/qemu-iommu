#!/bin/bash

sudo dmesg -n 8

function finddevs()
{
	ALLDEVS=$(ls /sys/bus/pci/devices)
	DEVS=
	# Find all TDISP devices
	for d in $ALLDEVS ; do
		if [ -e /sys/bus/pci/devices/$d/tsm/lock ] ; then
			DEVS="$DEVS $d"
		fi
	done
	echo $DEVS
}

function acceptall()
{
	for d in $* ; do
		sudo bash -c "echo tsm0 > \"/sys/bus/pci/devices/$d/tsm/lock\""
		./tsmsysfs.py -r /sys/bus/pci/devices/$d/tsm/report
		sudo bash -c "echo 1 > \"/sys/bus/pci/devices/$d/tsm/accept\""
	#	sudo bash -c "echo '1234 TEST MEAS NONCE' > /sys/bus/pci/devices/$d/tsm/meas_nonce"
	#	./tsmsysfs.py --tdi /sys/bus/pci/devices/$d/tsm/tdi_status
	done
}

function unlockall()
{
	for d in $* ; do
		sudo bash -c "echo tsm0 > \"/sys/bus/pci/devices/$d/tsm/unlock\""
		./tsmsysfs.py --tdi /sys/bus/pci/devices/$d/tsm/tdi_status
	done
}

function statusall()
{
	for d in $* ; do
		./tsmsysfs.py --tdi /sys/bus/pci/devices/$d/tsm/tdi_status
#	./tsmsysfs.py -c /sys/bus/pci/devices/$d/tsm/certs
#	./tsmsysfs.py -m /sys/bus/pci/devices/$d/tsm/meas
	done
}

function vfiobind()
{
        for dev in $* ; do
                 sudo bash -c "echo vfio-pci > /sys/bus/pci/devices/$dev/driver_override"
                 ( set -x ; sudo bash -c "echo $dev > /sys/bus/pci/drivers/vfio-pci/bind" )
                 sudo bash -c "echo '' > /sys/bus/pci/devices/$dev/driver_override"
        done
}

function unbind()
{
        for dev in $* ; do
                 sudo bash -c "echo '' > /sys/bus/pci/devices/$dev/driver_override"
                 sudo bash -c "echo $dev > /sys/bus/pci/devices/$dev/driver/unbind"
        done
}

function devtestall() {
	for d in $* ; do
		sudo ./pcimem/pcimem /sys/bus/pci/devices/$d/resource4_enc 0 d 0xdeadbeefbaadf00d
		sudo ./pcimem/pcimem /sys/bus/pci/devices/$d/resource4_enc 0 d 0
	done

	#for i in $(seq 0 1 100000) ; do
	for d in /dev/pcicdx/ksb_cdx_dev* ; do
		sudo /home/amd/ksb_pci_drv/user/test/dpu-test -f $d -d 0 -s 1024 -x
		sudo ./sev-guest  10
	done
}

DEVS=$(finddevs)
set -x

acceptall $DEVS ;
statusall $DEVS ;

devtestall $DEVS ;

unlockall $DEVS ;
exit 0
