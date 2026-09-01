# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: MIT
#
# The SMP/GATT twin of samples/9p_dfu_l2cap. Same board, same MCUboot, same
# slot layout, same signed image -- the ONLY difference is the DFU mechanism
# above the BLE link. Built to make "GATT or L2CAP?" a measurement, not an
# opinion.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smp_dfu_ble)

target_sources(app PRIVATE src/main.c)
