# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_NAMESPACE)

zephyr_library()

zephyr_library_sources(
	namespace.c
	ns_file_ops.c
)

if(CONFIG_NINEP_VFS)
	zephyr_library_sources(fs_9p.c)
endif()

if(CONFIG_SRV_REGISTRY)
	# TODO: Add srv.c when implemented
	# zephyr_library_sources(srv.c)
endif()

zephyr_library_include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

endif() # CONFIG_NAMESPACE
