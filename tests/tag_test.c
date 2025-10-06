/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/tag.h>
#include <zephyr/9p/protocol.h>

static struct ninep_tag_table tag_table;

static void *tag_setup(void)
{
	ninep_tag_table_init(&tag_table);
	return &tag_table;
}

/* Reset the tag table before each test */
static void tag_before(void *f)
{
	struct ninep_tag_table *table = (struct ninep_tag_table *)f;
	ninep_tag_table_init(table);
}

ZTEST_SUITE(ninep_tag, NULL, tag_setup, tag_before, NULL, NULL);

ZTEST(ninep_tag, test_tag_alloc)
{
	uint16_t tag;

	tag = ninep_tag_alloc(&tag_table);
	zassert_not_equal(tag, NINEP_NOTAG, "Failed to allocate tag");
	zassert_true(tag < CONFIG_NINEP_MAX_TAGS, "Tag out of range");
}

ZTEST(ninep_tag, test_tag_lookup)
{
	uint16_t tag;
	struct ninep_tag *found;

	tag = ninep_tag_alloc(&tag_table);
	zassert_not_equal(tag, NINEP_NOTAG, "Failed to allocate tag");

	found = ninep_tag_lookup(&tag_table, tag);
	zassert_not_null(found, "Lookup failed");
	zassert_equal(found->tag, tag, "Wrong tag");
}

ZTEST(ninep_tag, test_tag_free)
{
	uint16_t tag;
	struct ninep_tag *found;

	tag = ninep_tag_alloc(&tag_table);
	zassert_not_equal(tag, NINEP_NOTAG, "Failed to allocate tag");

	zassert_equal(ninep_tag_free(&tag_table, tag), 0, "Failed to free tag");

	found = ninep_tag_lookup(&tag_table, tag);
	zassert_is_null(found, "Tag still exists after free");
}

ZTEST(ninep_tag, test_tag_exhaustion)
{
	uint16_t tags[CONFIG_NINEP_MAX_TAGS + 1];
	int i;

	/* Allocate maximum tags */
	for (i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		tags[i] = ninep_tag_alloc(&tag_table);
		zassert_not_equal(tags[i], NINEP_NOTAG,
		                  "Failed to allocate tag %d", i);
	}

	/* Next allocation should fail */
	tags[i] = ninep_tag_alloc(&tag_table);
	zassert_equal(tags[i], NINEP_NOTAG, "Should fail when table full");

	/* Free one tag */
	zassert_equal(ninep_tag_free(&tag_table, tags[0]), 0,
	              "Failed to free tag");

	/* Now allocation should succeed */
	tags[0] = ninep_tag_alloc(&tag_table);
	zassert_not_equal(tags[0], NINEP_NOTAG,
	                  "Should succeed after freeing");
}

ZTEST(ninep_tag, test_tag_user_data)
{
	uint16_t tag;
	struct ninep_tag *tag_entry;
	void *test_data = (void *)0xCAFEBABE;

	tag = ninep_tag_alloc(&tag_table);
	zassert_not_equal(tag, NINEP_NOTAG, "Failed to allocate tag");

	tag_entry = ninep_tag_lookup(&tag_table, tag);
	zassert_not_null(tag_entry, "Failed to lookup tag");
	zassert_is_null(tag_entry->user_data, "user_data should be NULL initially");

	tag_entry->user_data = test_data;
	zassert_equal(tag_entry->user_data, test_data, "user_data not set correctly");

	/* Lookup again and verify persistence */
	tag_entry = ninep_tag_lookup(&tag_table, tag);
	zassert_equal(tag_entry->user_data, test_data, "user_data lost after lookup");
}

ZTEST(ninep_tag, test_tag_sequential_alloc)
{
	uint16_t tag1, tag2, tag3;

	tag1 = ninep_tag_alloc(&tag_table);
	tag2 = ninep_tag_alloc(&tag_table);
	tag3 = ninep_tag_alloc(&tag_table);

	zassert_not_equal(tag1, NINEP_NOTAG, "Failed to allocate tag1");
	zassert_not_equal(tag2, NINEP_NOTAG, "Failed to allocate tag2");
	zassert_not_equal(tag3, NINEP_NOTAG, "Failed to allocate tag3");

	/* Tags should be different */
	zassert_not_equal(tag1, tag2, "tag1 and tag2 should be different");
	zassert_not_equal(tag2, tag3, "tag2 and tag3 should be different");
	zassert_not_equal(tag1, tag3, "tag1 and tag3 should be different");
}

ZTEST(ninep_tag, test_tag_free_nonexistent)
{
	int ret;

	/* Try to free a tag that was never allocated */
	ret = ninep_tag_free(&tag_table, 999);
	zassert_not_equal(ret, 0, "Should fail when freeing non-existent tag");
}