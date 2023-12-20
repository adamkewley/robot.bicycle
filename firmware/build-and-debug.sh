#!/usr/bin/env bash

./build.sh && ~/toolchain/bin/arm-none-eabi-gdb -tui build/robot.bike.elf -x debug-commands.gdb
