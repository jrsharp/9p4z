/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_9P_FID_H_
#define ZEPHYR_INCLUDE_9P_FID_H_

#include <zephyr/9p/protocol.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_fid 9P File ID Management
 * @ingroup ninep
 * @{
 */

/**
 * @brief Fid entry state
 */
struct ninep_fid {
	uint32_t fid;           /* file ID number */
	struct ninep_qid qid;   /* qid associated with this fid */
	void *user_data;        /* user context pointer */
	bool in_use;            /* whether this fid is allocated */
};

/**
 * @brief Fid table for tracking open files
 */
struct ninep_fid_table {
	struct ninep_fid fids[CONFIG_NINEP_MAX_FIDS];
};

/**
 * @brief Initialize a fid table
 *
 * @param table Fid table to initialize
 */
void ninep_fid_table_init(struct ninep_fid_table *table);

/**
 * @brief Allocate a fid
 *
 * @param table Fid table
 * @param fid Fid number to allocate
 * @return Pointer to fid entry, or NULL if already in use or table full
 */
struct ninep_fid *ninep_fid_alloc(struct ninep_fid_table *table, uint32_t fid);

/**
 * @brief Look up a fid
 *
 * @param table Fid table
 * @param fid Fid number to look up
 * @return Pointer to fid entry, or NULL if not found
 */
struct ninep_fid *ninep_fid_lookup(struct ninep_fid_table *table, uint32_t fid);

/**
 * @brief Free a fid
 *
 * @param table Fid table
 * @param fid Fid number to free
 * @return 0 on success, negative error code on failure
 */
int ninep_fid_free(struct ninep_fid_table *table, uint32_t fid);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_FID_H_ */