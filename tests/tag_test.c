/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/tag.h>
#include <zephyr/9p/protocol.h>

static struct ninep_tag_table tag_table;

static void *tag_setup(void)
{
	ninep_tag_table_init(&tag_table);
	return NULL;
}

ZTEST_SUITE(ninep_tag, NULL, tag_setup, NULL, NULL, NULL);

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