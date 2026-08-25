#!/bin/bash
echo "Waiting for SD card..."
while true; do
    if lsblk -o NAME,LABEL | grep -q "BOOT"; then
        DEV=$(lsblk -o NAME,LABEL -p | grep "BOOT" | awk '{print $1}')
        if [ ! -z "$DEV" ]; then
            echo "Found SD card at $DEV! Mounting..."
            udisksctl mount -b $DEV
            MOUNTPOINT=$(lsblk -o MOUNTPOINTS -n $DEV)
            if [ -n "$MOUNTPOINT" ]; then
                echo "Copying files to $MOUNTPOINT..."
                cp build/kernel8.img "$MOUNTPOINT/"
                cp initrd.tar "$MOUNTPOINT/"
                sync
                echo "Unmounting..."
                udisksctl unmount -b $DEV
                echo "Successfully flashed! You can remove the SD card now."
                exit 0
            fi
        fi
    fi
    sleep 2
done
