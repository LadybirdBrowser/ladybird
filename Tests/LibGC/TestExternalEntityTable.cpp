/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibGC/ExternalEntityTable.h>
#include <LibTest/TestCase.h>

class TestExternalEntityTable final : private GC::ExternalEntityTable {
public:
    using ExternalEntityTable::allocate_entry;
    using ExternalEntityTable::free_entry;
    using ExternalEntityTable::is_valid;
};

TEST_CASE(balanced_tags_are_unique_and_have_equal_hamming_weight)
{
    constexpr size_t tag_count_to_test = 256;
    Array<GC::ExternalEntityTableTag, tag_count_to_test> tags;

    for (u16 ordinal = 0; ordinal < tags.size(); ++ordinal) {
        auto tag = GC::balanced_external_entity_table_tag(ordinal);
        EXPECT(tag.is_balanced());
        EXPECT_EQ(popcount(tag.value), GC::ExternalEntityTableTag::set_bit_count);
        for (u16 previous = 0; previous < ordinal; ++previous)
            EXPECT(tag != tags[previous]);
        tags[ordinal] = tag;
    }
}

TEST_CASE(entries_validate_their_type_and_generation)
{
    TestExternalEntityTable table;
    constexpr auto first_tag = GC::balanced_external_entity_table_tag(0);
    constexpr auto second_tag = GC::balanced_external_entity_table_tag(1);

    auto first_handle = table.allocate_entry(first_tag);
    EXPECT(table.is_valid(first_handle, first_tag));
    EXPECT(!table.is_valid(first_handle, second_tag));

    table.free_entry(first_handle, second_tag);
    EXPECT(table.is_valid(first_handle, first_tag));

    table.free_entry(first_handle, first_tag);
    EXPECT(!table.is_valid(first_handle, first_tag));

    auto second_handle = table.allocate_entry(second_tag);
    EXPECT_EQ(second_handle.index, first_handle.index);
    EXPECT_NE(second_handle.generation, first_handle.generation);
    EXPECT(table.is_valid(second_handle, second_tag));
    EXPECT(!table.is_valid(second_handle, first_tag));
}
