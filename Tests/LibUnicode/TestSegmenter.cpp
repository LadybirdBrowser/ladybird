/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibUnicode/Segmenter.h>

template<size_t N>
static void test_grapheme_segmentation(StringView string, size_t const (&expected_boundaries)[N])
{
    Vector<size_t> boundaries;
    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);

    segmenter->for_each_boundary(MUST(String::from_utf8(string)), [&](auto boundary) {
        boundaries.append(boundary);
        return IterationDecision::Continue;
    });

    EXPECT_EQ(boundaries, ReadonlySpan<size_t> { expected_boundaries });
}

TEST_CASE(grapheme_segmentation)
{
    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);

    segmenter->for_each_boundary(String {}, [&](auto i) {
        dbgln("{}", i);
        VERIFY_NOT_REACHED();
        return IterationDecision::Break;
    });

    test_grapheme_segmentation("a"sv, { 0u, 1u });
    test_grapheme_segmentation("ab"sv, { 0u, 1u, 2u });
    test_grapheme_segmentation("abc"sv, { 0u, 1u, 2u, 3u });

    test_grapheme_segmentation("a\nb"sv, { 0u, 1u, 2u, 3u });
    test_grapheme_segmentation("a\n\rb"sv, { 0u, 1u, 2u, 3u, 4u });
    test_grapheme_segmentation("a\r\nb"sv, { 0u, 1u, 3u, 4u });

    test_grapheme_segmentation("aᄀb"sv, { 0u, 1u, 4u, 5u });
    test_grapheme_segmentation("aᄀᄀb"sv, { 0u, 1u, 7u, 8u });
    test_grapheme_segmentation("aᄀᆢb"sv, { 0u, 1u, 7u, 8u });
    test_grapheme_segmentation("aᄀ가b"sv, { 0u, 1u, 7u, 8u });
    test_grapheme_segmentation("aᄀ각b"sv, { 0u, 1u, 7u, 8u });

    test_grapheme_segmentation("a😀b"sv, { 0u, 1u, 5u, 6u });
    test_grapheme_segmentation("a👨‍👩‍👧‍👦b"sv, { 0u, 1u, 26u, 27u });
    test_grapheme_segmentation("a👩🏼‍❤️‍👨🏻b"sv, { 0u, 1u, 29u, 30u });
}

TEST_CASE(grapheme_segmentation_indic_conjunct_break)
{
    test_grapheme_segmentation("\u0915"sv, { 0u, 3u });
    test_grapheme_segmentation("\u0915a"sv, { 0u, 3u, 4u });
    test_grapheme_segmentation("\u0915\u0916"sv, { 0u, 3u, 6u });

    test_grapheme_segmentation("\u0915\u094D\u0916"sv, { 0u, 9u });

    test_grapheme_segmentation("\u0915\u09BC\u09CD\u094D\u0916"sv, { 0u, 15u });
    test_grapheme_segmentation("\u0915\u094D\u09BC\u09CD\u0916"sv, { 0u, 15u });

    test_grapheme_segmentation("\u0915\u09BC\u09CD\u094D\u09BC\u09CD\u0916"sv, { 0u, 21u });
    test_grapheme_segmentation("\u0915\u09BC\u09CD\u09BC\u09CD\u094D\u0916"sv, { 0u, 21u });
    test_grapheme_segmentation("\u0915\u094D\u09BC\u09CD\u09BC\u09CD\u0916"sv, { 0u, 21u });

    test_grapheme_segmentation("\u0915\u09BC\u09CD\u09BC\u09CD\u094D\u09BC\u09CD\u0916"sv, { 0u, 27u });
    test_grapheme_segmentation("\u0915\u09BC\u09CD\u094D\u09BC\u09CD\u09BC\u09CD\u0916"sv, { 0u, 27u });

    test_grapheme_segmentation("\u0915\u09BC\u09CD\u09BC\u09CD\u094D\u09BC\u09CD\u09BC\u09CD\u0916"sv, { 0u, 33u });
}

template<size_t N>
static void test_word_segmentation(StringView string, size_t const (&expected_boundaries)[N])
{
    Vector<size_t> boundaries;
    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Word);

    segmenter->for_each_boundary(MUST(String::from_utf8(string)), [&](auto boundary) {
        boundaries.append(boundary);
        return IterationDecision::Continue;
    });

    EXPECT_EQ(boundaries, ReadonlySpan<size_t> { expected_boundaries });
}

TEST_CASE(word_segmentation)
{
    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Word);

    segmenter->for_each_boundary(String {}, [&](auto) {
        VERIFY_NOT_REACHED();
        return IterationDecision::Break;
    });

    test_word_segmentation("a"sv, { 0u, 1u });
    test_word_segmentation("ab"sv, { 0u, 2u });
    test_word_segmentation("abc"sv, { 0u, 3u });

    test_word_segmentation("ab cd"sv, { 0u, 2u, 3u, 5u });
    test_word_segmentation("ab  cd"sv, { 0u, 2u, 4u, 6u });
    test_word_segmentation("ab\tcd"sv, { 0u, 2u, 3u, 5u });
    test_word_segmentation("ab\ncd"sv, { 0u, 2u, 3u, 5u });
    test_word_segmentation("ab\n\rcd"sv, { 0u, 2u, 3u, 4u, 6u });
    test_word_segmentation("ab\r\ncd"sv, { 0u, 2u, 4u, 6u });

    test_word_segmentation("a😀b"sv, { 0u, 1u, 5u, 6u });
    test_word_segmentation("a👨‍👩‍👧‍👦b"sv, { 0u, 1u, 26u, 27u });
    test_word_segmentation("a👩🏼‍❤️‍👨🏻b"sv, { 0u, 1u, 29u, 30u });

    test_word_segmentation("ab 12 cd"sv, { 0u, 2u, 3u, 5u, 6u, 8u });
    test_word_segmentation("ab 1.2 cd"sv, { 0u, 2u, 3u, 6u, 7u, 9u });
    test_word_segmentation("ab 12.34 cd"sv, { 0u, 2u, 3u, 8u, 9u, 11u });
    test_word_segmentation("ab example.com cd"sv, { 0u, 2u, 3u, 14u, 15u, 17u });

    test_word_segmentation("ab can't cd"sv, { 0u, 2u, 3u, 8u, 9u, 11u });
    test_word_segmentation("ab \"can't\" cd"sv, { 0u, 2u, 3u, 4u, 9u, 10u, 11u, 13u });

    test_word_segmentation(
        "The quick (“brown”) fox can’t jump 32.3 feet, right?"sv,
        { 0u, 3u, 4u, 9u, 10u, 11u, 14u, 19u, 22u, 23u, 24u, 27u, 28u, 35u, 36u, 40u, 41u, 45u, 46u, 50u, 51u, 52u, 57u, 58u });
}
