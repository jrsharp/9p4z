/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/fid.h>

static struct ninep_fid_table fid_table;

static void *fid_setup(void)
{
	ninep_fid_table_init(&fid_table);
	return NULL;
}

ZTEST_SUITE(ninep_fid, NULL, fid_setup, NULL, NULL, NULL);

ZTEST(ninep_fid, test_fid_alloc)
{
	struct ninep_fid *fid;

	fid = ninep_fid_alloc(&fid_table, 1);
	zassert_not_null(fid, "Failed to allocate fid");
	zassert_equal(fid->fid, 1, "Wrong fid number");
	zassert_true(fid->in_use, "Fid not marked in use");
}

ZTEST(ninep_fid, test_fid_lookup)
{
	struct ninep_fid *fid, *found;

	fid = ninep_fid_alloc(&fid_table, 2);
	zassert_not_null(fid, "Failed to allocate fid");

	found = ninep_fid_lookup(&fid_table, 2);
	zassert_equal(found, fid, "Lookup returned wrong fid");

	found = ninep_fid_lookup(&fid_table, 999);
	zassert_is_null(found, "Lookup should return NULL for non-existent fid");
}

ZTEST(ninep_fid, test_fid_free)
{
	struct ninep_fid *fid, *found;

	fid = ninep_fid_alloc(&fid_table, 3);
	zassert_not_null(fid, "Failed to allocate fid");

	zassert_equal(ninep_fid_free(&fid_table, 3), 0, "Failed to free fid");

	found = ninep_fid_lookup(&fid_table, 3);
	zassert_is_null(found, "Fid still exists after free");
}

ZTEST(ninep_fid, test_fid_duplicate)
{
	struct ninep_fid *fid1, *fid2;

	fid1 = ninep_fid_alloc(&fid_table, 4);
	zassert_not_null(fid1, "Failed to allocate fid");

	fid2 = ninep_fid_alloc(&fid_table, 4);
	zassert_is_null(fid2, "Should not allocate duplicate fid");
}