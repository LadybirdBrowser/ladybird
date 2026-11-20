/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/StringBuilder.h>
#include <AK/UnicodeUtils.h>
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

TEST_CASE(search_keys_exist_at_base_and_variant_sensitivity_only)
{
    EXPECT(Unicode::search_key_for_code_point('a', Unicode::Sensitivity::Base).has_value());
    EXPECT(Unicode::search_key_for_code_point('a', Unicode::Sensitivity::Variant).has_value());
    EXPECT(!Unicode::search_key_for_code_point('a', Unicode::Sensitivity::Accent).has_value());
    EXPECT(!Unicode::search_key_for_code_point('a', Unicode::Sensitivity::Case).has_value());
}

TEST_CASE(search_keys_split_text_into_code_points_one_way)
{
    HashTable<u32> single_elements;
    HashTable<u32> long_weight_first_halves;
    for (u32 code_point = 0; code_point <= 0x10ffff; ++code_point) {
        auto key = Unicode::search_key_for_code_point(code_point, Unicode::Sensitivity::Base);
        if (!key.has_value())
            continue;
        auto first_element = static_cast<u32>(*key >> 32);
        if (static_cast<u32>(*key) == 0) {
            single_elements.set(first_element);
        } else {
            long_weight_first_halves.set(first_element);
        }
    }

    EXPECT(!single_elements.is_empty());
    EXPECT(!long_weight_first_halves.is_empty());
    for (auto first_half : long_weight_first_halves) {
        if (single_elements.contains(first_half))
            FAIL(ByteString::formatted("{:08x} is the first half of a long weight and the weight of a code point", first_half));
    }
}

extern "C" {
void* unicode_layout_text_searcher_create(u16 const*, size_t, bool);
bool unicode_layout_text_is_searchable_without_collator(u16 const*, size_t);
void unicode_layout_text_searcher_set_text(void*, u16 const*, size_t, size_t, bool);
bool unicode_layout_text_searcher_next_match(void*, size_t*, size_t*);
void unicode_layout_text_searcher_destroy(void*);
}

static ByteString describe(Vector<char16_t> const& text)
{
    StringBuilder builder;
    for (auto code_point : Utf16View { text.data(), text.size() })
        builder.appendff(" U+{:04X}", code_point);
    return builder.to_byte_string();
}

static void expect_layout_search_finds_what_the_collator_finds(Unicode::Sensitivity sensitivity, Unicode::Collator const& collator, Vector<char16_t> const& needle, Vector<Vector<char16_t>> const& haystacks)
{
    auto reference = collator.create_needle_searcher({ needle.data(), needle.size() });
    auto* searcher = unicode_layout_text_searcher_create(reinterpret_cast<u16 const*>(needle.data()), needle.size(), sensitivity == Unicode::Sensitivity::Variant);
    for (auto const& haystack : haystacks) {
        Vector<Unicode::Collator::Match> expected;
        reference->set_haystack({ haystack.data(), haystack.size() });
        for (auto match = reference->next_match(); match.has_value(); match = reference->next_match())
            expected.append(*match);

        Vector<Unicode::Collator::Match> found;
        auto const* code_units = reinterpret_cast<u16 const*>(haystack.data());
        unicode_layout_text_searcher_set_text(searcher, code_units, haystack.size(), 0, unicode_layout_text_is_searchable_without_collator(code_units, haystack.size()));
        for (size_t start = 0, length = 0; unicode_layout_text_searcher_next_match(searcher, &start, &length);)
            found.append({ start, start + length });

        auto same_matches = expected.size() == found.size();
        for (size_t index = 0; same_matches && index < expected.size(); ++index)
            same_matches = expected[index].start == found[index].start && expected[index].end == found[index].end;
        if (!same_matches)
            FAIL(ByteString::formatted("Needle{} in haystack{} at sensitivity {}", describe(needle), describe(haystack), to_underlying(sensitivity)));
    }
    unicode_layout_text_searcher_destroy(searcher);
}

TEST_CASE(search_keys_compare_text_as_the_collator_does)
{
    struct CodePointRange {
        u32 first;
        u32 last;
    };
    // Kana letters also have to agree in size and voicing, which the collator does not check.
    static constexpr Array ranges {
        CodePointRange { 0x9, 0xd },
        CodePointRange { 0x20, 0x7e },
        CodePointRange { 0xa0, 0x24f },
        CodePointRange { 0x370, 0x4ff },
        CodePointRange { 0x5d0, 0x5ea },
        CodePointRange { 0x620, 0x64a },
        CodePointRange { 0x1e00, 0x1fff },
        CodePointRange { 0x2000, 0x218f },
        CodePointRange { 0x2460, 0x24ff },
        CodePointRange { 0x3131, 0x318e },
        CodePointRange { 0x3358, 0x33ff },
        CodePointRange { 0x4e00, 0x4fff },
        CodePointRange { 0xf900, 0xfb4f },
        CodePointRange { 0xfe70, 0xfefc },
        CodePointRange { 0xff00, 0xff65 },
        CodePointRange { 0xffa0, 0xffef },
        CodePointRange { 0x1d400, 0x1d4ff },
        CodePointRange { 0x1f600, 0x1f64f },
        CodePointRange { 0x20000, 0x200ff },
    };

    u64 random_state = 1;
    auto random = [&](size_t bound) {
        random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<size_t>(random_state >> 33) % bound;
    };

    for (auto sensitivity : { Unicode::Sensitivity::Base, Unicode::Sensitivity::Variant }) {
        HashMap<u64, Vector<u32>> code_points_by_key;
        for (auto range : ranges) {
            for (auto code_point = range.first; code_point <= range.last; ++code_point) {
                auto key_ignoring_case = Unicode::search_key_for_code_point(code_point, Unicode::Sensitivity::Base);
                auto key_matching_case = Unicode::search_key_for_code_point(code_point, Unicode::Sensitivity::Variant);
                if (key_ignoring_case.has_value() && key_matching_case.has_value())
                    code_points_by_key.ensure(sensitivity == Unicode::Sensitivity::Base ? *key_ignoring_case : *key_matching_case).append(code_point);
            }
        }
        Vector<Vector<u32>> classes;
        Vector<u64> class_keys;
        Vector<size_t> classes_with_several_members;
        Vector<size_t> classes_with_several_elements;
        HashMap<u32, Vector<size_t>> single_element_classes_by_element;
        for (auto& it : code_points_by_key) {
            if (it.value.size() > 1)
                classes_with_several_members.append(classes.size());
            if (static_cast<u32>(it.key) != 0) {
                classes_with_several_elements.append(classes.size());
            } else {
                single_element_classes_by_element.ensure(static_cast<u32>(it.key >> 32)).append(classes.size());
            }
            class_keys.append(it.key);
            classes.append(move(it.value));
        }
        EXPECT(!classes_with_several_members.is_empty());
        EXPECT(!classes_with_several_elements.is_empty());

        auto collator = Unicode::Collator::create({}, Unicode::Usage::Search, {}, sensitivity, Unicode::CaseFirst::False, false, false);
        for (size_t trial = 0; trial < 4000; ++trial) {
            Vector<size_t> alphabet_classes;
            for (size_t class_count = 1 + random(3); class_count > 0; --class_count) {
                size_t class_index = 0;
                switch (random(3)) {
                case 0:
                    class_index = random(classes.size());
                    break;
                case 1:
                    class_index = classes_with_several_members[random(classes_with_several_members.size())];
                    break;
                default:
                    class_index = classes_with_several_elements[random(classes_with_several_elements.size())];
                    break;
                }
                alphabet_classes.append(class_index);
                for (auto element : { static_cast<u32>(class_keys[class_index] >> 32), static_cast<u32>(class_keys[class_index]) }) {
                    if (auto related_classes = single_element_classes_by_element.get(element); related_classes.has_value())
                        alphabet_classes.extend(*related_classes);
                }
            }
            Vector<u32> alphabet;
            for (auto class_index : alphabet_classes) {
                auto const& members = classes[class_index];
                for (size_t member = 0; member < 3; ++member)
                    alphabet.append(members[random(members.size())]);
            }
            auto random_text = [&](size_t length) {
                Vector<char16_t> text;
                for (size_t index = 0; index < length; ++index)
                    (void)AK::UnicodeUtils::code_point_to_utf16(alphabet[random(alphabet.size())], [&](char16_t code_unit) { text.append(code_unit); });
                return text;
            };

            auto needle = random_text(1 + random(4));
            Vector<Vector<char16_t>> haystacks;
            for (size_t haystack_index = 0; haystack_index < 10; ++haystack_index) {
                auto haystack = random_text(1 + random(24));
                EXPECT(unicode_layout_text_is_searchable_without_collator(reinterpret_cast<u16 const*>(haystack.data()), haystack.size()));
                haystacks.append(move(haystack));
            }
            expect_layout_search_finds_what_the_collator_finds(sensitivity, *collator, needle, haystacks);
        }
    }
}

TEST_CASE(search_keys_leave_contractions_to_the_collator)
{
    static constexpr Array<char16_t, 7> alphabet { 'a', 'L', 'l', 0xb7, 0x387, 0x13f, 0x140 };

    Vector<Vector<char16_t>> texts;
    for (auto first : alphabet) {
        texts.append({ first });
        for (auto second : alphabet) {
            texts.append({ first, second });
            for (auto third : alphabet)
                texts.append({ first, second, third });
        }
    }

    for (auto sensitivity : { Unicode::Sensitivity::Base, Unicode::Sensitivity::Variant }) {
        auto collator = Unicode::Collator::create({}, Unicode::Usage::Search, {}, sensitivity, Unicode::CaseFirst::False, false, false);
        for (auto const& needle : texts) {
            if (needle.size() <= 2)
                expect_layout_search_finds_what_the_collator_finds(sensitivity, *collator, needle, texts);
        }
    }
}
