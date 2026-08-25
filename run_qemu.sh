#!/bin/bash
(
  echo "./pty.elf"
  sleep 2
) | make run > qemu.log &
QEMU_PID=$!
sleep 5
kill -9 $QEMU_PID
cat qemu.log | tail -n 25
