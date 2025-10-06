/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/tag.h>
#include <zephyr/9p/protocol.h>
#include <string.h>
#include <errno.h>

void ninep_tag_table_init(struct ninep_tag_table *table)
{
	if (!table) {
		return;
	}

	memset(table, 0, sizeof(*table));
}

uint16_t ninep_tag_alloc(struct ninep_tag_table *table)
{
	if (!table) {
		return NINEP_NOTAG;
	}

	/* Find free slot */
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		if (!table->tags[i].in_use) {
			table->tags[i].tag = i;
			table->tags[i].in_use = true;
			table->tags[i].user_data = NULL;
			return i;
		}
	}

	return NINEP_NOTAG;  /* table full */
}

struct ninep_tag *ninep_tag_lookup(struct ninep_tag_table *table, uint16_t tag)
{
	if (!table || tag >= CONFIG_NINEP_MAX_TAGS) {
		return NULL;
	}

	if (table->tags[tag].in_use) {
		return &table->tags[tag];
	}

	return NULL;
}

int ninep_tag_free(struct ninep_tag_table *table, uint16_t tag)
{
	if (!table || tag >= CONFIG_NINEP_MAX_TAGS) {
		return -EINVAL;
	}

	if (table->tags[tag].in_use) {
		table->tags[tag].in_use = false;
		return 0;
	}

	return -ENOENT;
}