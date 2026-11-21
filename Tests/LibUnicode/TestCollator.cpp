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

static void expect_matches(Unicode::Sensitivity sensitivity, Utf16View needle, Utf16View haystack, Vector<Unicode::Collator::Match> const& expected)
{
    auto collator = Unicode::Collator::create({}, Unicode::Usage::Search, {}, sensitivity, Unicode::CaseFirst::False, false, false);
    auto searcher = collator->create_needle_searcher(needle);
    searcher->set_haystack(haystack);

    for (auto const& expected_match : expected) {
        auto match = searcher->next_match();
        VERIFY(match.has_value());
        EXPECT_EQ(match->start, expected_match.start);
        EXPECT_EQ(match->end, expected_match.end);
    }
    EXPECT(!searcher->next_match().has_value());
}

TEST_CASE(needle_search_at_base_sensitivity)
{
    using enum Unicode::Sensitivity;

    expect_matches(Base, u"needle"sv, u"Needle NEEDLE needle"sv, { { 0, 6 }, { 7, 13 }, { 14, 20 } });
    expect_matches(Base, u"\u00e9cole"sv, u"\u00c9COLE"sv, { { 0, 5 } });
    expect_matches(Base, u"\u03c3\u03af\u03c3\u03c5\u03c6\u03bf\u03c2"sv, u"\u03a3\u038a\u03a3\u03a5\u03a6\u039f\u03a3"sv, { { 0, 7 } });
    expect_matches(Base, u"cafe"sv, u"caf\u00e9 cafe\u0301"sv, { { 0, 4 }, { 5, 10 } });
    expect_matches(Base, u"strasse"sv, u"stra\u00dfe"sv, { { 0, 6 } });
    expect_matches(Base, u"needle"sv, u"\uff4e\uff45\uff45\uff44\uff4c\uff45"sv, { { 0, 6 } });
    expect_matches(Base, u"foo bar"sv, u"foo\u00a0bar"sv, { { 0, 7 } });
    expect_matches(Base, u"needle"sv, u"nee\u00addle nee\u200bdle nee\u200ddle"sv, { { 0, 7 }, { 8, 15 }, { 16, 23 } });
    expect_matches(Base, u"foo"sv, u"foofoo"sv, { { 0, 3 }, { 3, 6 } });
}

TEST_CASE(needle_search_at_variant_sensitivity)
{
    using enum Unicode::Sensitivity;

    expect_matches(Variant, u"needle"sv, u"Needle NEEDLE needle"sv, { { 14, 20 } });
    expect_matches(Variant, u"cafe"sv, u"caf\u00e9 cafe"sv, { { 5, 9 } });
    expect_matches(Variant, u"caf\u00e9"sv, u"cafe\u0301"sv, { { 0, 5 } });
    expect_matches(Variant, u"needle"sv, u"nee\u00addle"sv, { { 0, 7 } });
}

TEST_CASE(needle_search_in_one_haystack_after_another)
{
    auto collator = create_search_collator();
    auto searcher = collator->create_needle_searcher(u"b"sv);
    EXPECT(!searcher->next_match().has_value());

    searcher->set_haystack(u"abc"sv);
    auto match = searcher->next_match();
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 1u);
    EXPECT(!searcher->next_match().has_value());

    searcher->set_haystack({});
    EXPECT(!searcher->next_match().has_value());

    auto latin1_haystack = Utf16String::from_utf8("B\u00e9b"sv);
    searcher->set_haystack(latin1_haystack);
    match = searcher->next_match();
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 0u);
    match = searcher->next_match();
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 2u);

    EXPECT(!collator->create_needle_searcher({})->next_match().has_value());
}

TEST_CASE(needle_search_from_an_offset)
{
    auto collator = create_search_collator();
    auto searcher = collator->create_needle_searcher(u"aa"sv);
    EXPECT(!searcher->find_from(0).has_value());

    searcher->set_haystack(u"aaa aa"sv);
    auto match = searcher->find_from(1);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 1u);
    EXPECT_EQ(match->end, 3u);

    match = searcher->next_match();
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 4u);
    EXPECT(!searcher->next_match().has_value());

    EXPECT(!searcher->find_from(5).has_value());
    EXPECT(!searcher->find_from(100).has_value());

    match = searcher->find_from(0);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 0u);
    EXPECT(!searcher->find_from(6).has_value());
    EXPECT(!searcher->next_match().has_value());

    auto emoji_searcher = collator->create_needle_searcher(u"\U0001FAE9"sv);
    auto haystack = Utf16String::from_utf8("\U0001FAE9 \U0001FAE9"sv);
    emoji_searcher->set_haystack(haystack);
    match = emoji_searcher->find_from(1);
    VERIFY(match.has_value());
    EXPECT_EQ(match->start, 3u);
}

TEST_CASE(needle_search_compares_printable_ascii_as_plain_text)
{
    Vector<char16_t> printable;
    for (char16_t code_unit = 0x20; code_unit <= 0x7e; ++code_unit)
        printable.append(code_unit);

    static constexpr char16_t no_break_space = 0xa0;
    Vector<char16_t> spaces { u'\t', u'\n', u'\v', u'\f', u'\r', no_break_space };
    auto printable_and_spaces = printable;
    printable_and_spaces.extend(spaces);

    for (auto sensitivity : { Unicode::Sensitivity::Base, Unicode::Sensitivity::Variant }) {
        auto collator = Unicode::Collator::create({}, Unicode::Usage::Search, {}, sensitivity, Unicode::CaseFirst::False, false, false);
        auto are_equal = [&](char16_t needle, char16_t code_unit) {
            if (sensitivity == Unicode::Sensitivity::Variant)
                return needle == code_unit;
            if (code_unit == no_break_space)
                return needle == u' ';
            return to_ascii_lowercase(needle) == to_ascii_lowercase(code_unit);
        };

        for (auto needle : printable) {
            auto searcher = collator->create_needle_searcher({ &needle, 1 });
            for (auto first : printable_and_spaces) {
                for (auto second : printable_and_spaces) {
                    char16_t haystack[] { first, second };
                    searcher->set_haystack({ haystack, 2 });

                    Vector<size_t> expected;
                    if (are_equal(needle, first))
                        expected.append(0);
                    if (are_equal(needle, second))
                        expected.append(1);

                    Vector<size_t> found;
                    for (auto match = searcher->next_match(); match.has_value(); match = searcher->next_match()) {
                        EXPECT_EQ(match->end, match->start + 1);
                        found.append(match->start);
                    }
                    if (found != expected)
                        FAIL(ByteString::formatted("U+{:04X} in U+{:04X} U+{:04X} at sensitivity {}", static_cast<u32>(needle), static_cast<u32>(first), static_cast<u32>(second), to_underlying(sensitivity)));
                }
            }
        }

        for (auto first : printable) {
            for (auto second : printable) {
                char16_t needle[] { first, second };
                auto searcher = collator->create_needle_searcher({ needle, 2 });
                for (auto space : spaces) {
                    char16_t haystack[] { first, space, second };
                    searcher->set_haystack({ haystack, 3 });
                    auto matches_through_space = are_equal(second, space) || are_equal(first, space);
                    if (searcher->next_match().has_value() != matches_through_space)
                        FAIL(ByteString::formatted("U+{:04X} U+{:04X} across U+{:04X} at sensitivity {}", static_cast<u32>(first), static_cast<u32>(second), static_cast<u32>(space), to_underlying(sensitivity)));
                }
            }
        }
    }
}
