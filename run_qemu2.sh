#!/bin/bash
make run > qemu2.log &
QEMU_PID=$!
sleep 2
kill -9 $QEMU_PID
cat qemu2.log | grep -A 20 "Starting scheduler"
