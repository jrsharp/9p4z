# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ninep_tests)

target_sources(app PRIVATE
  protocol_test.c
  fid_test.c
  tag_test.c
  transport_test.c
  message_test.c
  message_builder_test.c
  uart_transport_test.c
  client_server_test.c
  stress_test.c
)