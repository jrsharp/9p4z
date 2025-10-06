/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/fid.h>

static struct ninep_fid_table fid_table;

static void *fid_setup(void)
{
	ninep_fid_table_init(&fid_table);
	return &fid_table;
}

/* Reset the fid table before each test */
static void fid_before(void *f)
{
	struct ninep_fid_table *table = (struct ninep_fid_table *)f;
	ninep_fid_table_init(table);
}

ZTEST_SUITE(ninep_fid, NULL, fid_setup, fid_before, NULL, NULL);

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

ZTEST(ninep_fid, test_fid_exhaustion)
{
	struct ninep_fid *fids[CONFIG_NINEP_MAX_FIDS + 1];
	int i;

	/* Allocate maximum number of fids */
	for (i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		fids[i] = ninep_fid_alloc(&fid_table, i);
		zassert_not_null(fids[i], "Failed to allocate fid %d", i);
	}

	/* Next allocation should fail */
	fids[i] = ninep_fid_alloc(&fid_table, i);
	zassert_is_null(fids[i], "Should fail when table is full");

	/* Free one fid */
	zassert_equal(ninep_fid_free(&fid_table, 0), 0, "Failed to free fid");

	/* Now allocation should succeed */
	fids[0] = ninep_fid_alloc(&fid_table, CONFIG_NINEP_MAX_FIDS);
	zassert_not_null(fids[0], "Should succeed after freeing a fid");
}

ZTEST(ninep_fid, test_fid_reuse)
{
	struct ninep_fid *fid1, *fid2;

	fid1 = ninep_fid_alloc(&fid_table, 100);
	zassert_not_null(fid1, "Failed to allocate fid");

	zassert_equal(ninep_fid_free(&fid_table, 100), 0, "Failed to free fid");

	/* Reuse the same fid number */
	fid2 = ninep_fid_alloc(&fid_table, 100);
	zassert_not_null(fid2, "Failed to reallocate fid");
	zassert_equal(fid2->fid, 100, "Wrong fid number");
}

ZTEST(ninep_fid, test_fid_user_data)
{
	struct ninep_fid *fid;
	void *test_data = (void *)0xDEADBEEF;

	fid = ninep_fid_alloc(&fid_table, 200);
	zassert_not_null(fid, "Failed to allocate fid");
	zassert_is_null(fid->user_data, "user_data should be NULL initially");

	fid->user_data = test_data;
	zassert_equal(fid->user_data, test_data, "user_data not set correctly");

	/* Lookup and verify user_data persists */
	fid = ninep_fid_lookup(&fid_table, 200);
	zassert_not_null(fid, "Failed to lookup fid");
	zassert_equal(fid->user_data, test_data, "user_data lost after lookup");
}

ZTEST(ninep_fid, test_fid_qid_storage)
{
	struct ninep_fid *fid;
	struct ninep_qid qid = {
		.type = 0x80,
		.version = 1,
		.path = 0x42,
	};

	fid = ninep_fid_alloc(&fid_table, 300);
	zassert_not_null(fid, "Failed to allocate fid");

	/* Store qid */
	fid->qid = qid;

	/* Lookup and verify qid */
	fid = ninep_fid_lookup(&fid_table, 300);
	zassert_not_null(fid, "Failed to lookup fid");
	zassert_equal(fid->qid.type, qid.type, "qid type mismatch");
	zassert_equal(fid->qid.version, qid.version, "qid version mismatch");
	zassert_equal(fid->qid.path, qid.path, "qid path mismatch");
}