/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TAG_H_
#define ZEPHYR_INCLUDE_9P_TAG_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_tag 9P Tag Management
 * @ingroup ninep
 * @{
 */

/**
 * @brief Tag entry state
 */
struct ninep_tag {
	uint16_t tag;      /* tag number */
	void *user_data;   /* user context pointer */
	bool in_use;       /* whether this tag is allocated */
};

/**
 * @brief Tag table for tracking pending requests
 */
struct ninep_tag_table {
	struct ninep_tag tags[CONFIG_NINEP_MAX_TAGS];
};

/**
 * @brief Initialize a tag table
 *
 * @param table Tag table to initialize
 */
void ninep_tag_table_init(struct ninep_tag_table *table);

/**
 * @brief Allocate a tag
 *
 * @param table Tag table
 * @return Tag number, or NINEP_NOTAG if table is full
 */
uint16_t ninep_tag_alloc(struct ninep_tag_table *table);

/**
 * @brief Look up a tag
 *
 * @param table Tag table
 * @param tag Tag number to look up
 * @return Pointer to tag entry, or NULL if not found
 */
struct ninep_tag *ninep_tag_lookup(struct ninep_tag_table *table, uint16_t tag);

/**
 * @brief Free a tag
 *
 * @param table Tag table
 * @param tag Tag number to free
 * @return 0 on success, negative error code on failure
 */
int ninep_tag_free(struct ninep_tag_table *table, uint16_t tag);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TAG_H_ */