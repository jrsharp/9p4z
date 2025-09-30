# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_NINEP)

zephyr_library()

zephyr_include_directories(include)

zephyr_library_sources(
  src/proto.c
  src/fid.c
  src/tag.c
)

if(CONFIG_NINEP_SERVER)
  zephyr_library_sources(src/server.c)
endif()

if(CONFIG_NINEP_CLIENT)
  zephyr_library_sources(src/client.c)
endif()

if(CONFIG_NINEP_TRANSPORT_UART)
  zephyr_library_sources(src/transport_uart.c)
endif()

if(CONFIG_NINEP_TRANSPORT_TCP)
  zephyr_library_sources(src/transport_tcp.c)
endif()

if(CONFIG_NINEP_TRANSPORT_L2CAP)
  zephyr_library_sources(src/transport_l2cap.c)
endif()

endif() # CONFIG_NINEP