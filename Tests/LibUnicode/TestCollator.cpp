/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Utf16String.h>
#include <LibUnicode/Collator.h>

static NonnullOwnPtr<Unicode::Collator> create_search_collator()
{
    return Unicode::Collator::create({}, Unicode::Usage::Search, {}, Unicode::Sensitivity::Base, Unicode::CaseFirst::False, false, false);
}

TEST_CASE(substring_search_from_inside_a_surrogate_pair)
{
    auto collator = create_search_collator();
    auto haystack = Utf16String::from_utf8("a \U0001FAE9 b \U0001FAE9"sv);
    auto searcher = collator->create_substring_searcher(haystack, u"\U0001FAE9"sv);

    auto match = searcher->find_from(0);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 2u);
    EXPECT_EQ(match->end, 4u);

    match = searcher->find_from(3);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 7u);
    EXPECT_EQ(match->end, 9u);

    EXPECT(!searcher->find_from(8).has_value());
}

TEST_CASE(substring_search_finds_overlapping_match_after_a_surrogate_pair)
{
    auto collator = create_search_collator();
    auto haystack = Utf16String::from_utf8("\U0001FAE9\U0001FAE9\U0001FAE9"sv);
    auto searcher = collator->create_substring_searcher(haystack, u"\U0001FAE9\U0001FAE9"sv);

    auto match = searcher->find_from(1);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 2u);
    EXPECT_EQ(match->end, 6u);
}

TEST_CASE(substring_search_from_inside_the_base_of_an_emoji_modifier_sequence)
{
    auto collator = create_search_collator();
    auto haystack = Utf16String::from_utf8("\U0001F44D\U0001F3FD \U0001F44D"sv);
    auto searcher = collator->create_substring_searcher(haystack, u"\U0001F44D"sv);

    auto match = searcher->find_from(0);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 0u);
    EXPECT_EQ(match->end, 2u);

    match = searcher->find_from(1);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 5u);
    EXPECT_EQ(match->end, 7u);
}

TEST_CASE(substring_search_finds_overlapping_match_after_a_contraction)
{
    auto collator = create_search_collator();
    auto haystack = Utf16String::from_utf8("=\u0338=\u0338=\u0338"sv);
    auto searcher = collator->create_substring_searcher(haystack, u"=\u0338=\u0338"sv);

    auto match = searcher->find_from(1);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 2u);
    EXPECT_EQ(match->end, 6u);

    EXPECT(!searcher->find_from(3).has_value());
}
