/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/fid.h>
#include <string.h>
#include <errno.h>

void ninep_fid_table_init(struct ninep_fid_table *table)
{
	if (!table) {
		return;
	}

	memset(table, 0, sizeof(*table));
}

struct ninep_fid *ninep_fid_alloc(struct ninep_fid_table *table, uint32_t fid)
{
	if (!table) {
		return NULL;
	}

	/* Check if fid already exists */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (table->fids[i].in_use && table->fids[i].fid == fid) {
			return NULL;  /* fid already in use */
		}
	}

	/* Find free slot */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!table->fids[i].in_use) {
			table->fids[i].fid = fid;
			table->fids[i].in_use = true;
			table->fids[i].user_data = NULL;
			memset(&table->fids[i].qid, 0, sizeof(struct ninep_qid));
			return &table->fids[i];
		}
	}

	return NULL;  /* table full */
}

struct ninep_fid *ninep_fid_lookup(struct ninep_fid_table *table, uint32_t fid)
{
	if (!table) {
		return NULL;
	}

	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (table->fids[i].in_use && table->fids[i].fid == fid) {
			return &table->fids[i];
		}
	}

	return NULL;
}

int ninep_fid_free(struct ninep_fid_table *table, uint32_t fid)
{
	if (!table) {
		return -EINVAL;
	}

	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (table->fids[i].in_use && table->fids[i].fid == fid) {
			table->fids[i].in_use = false;
			return 0;
		}
	}

	return -ENOENT;
}