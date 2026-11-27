/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
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

template<size_t N>
static void test_line_segmentation(StringView string, size_t const (&expected_boundaries)[N])
{
    Vector<size_t> boundaries;
    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Line);

    segmenter->for_each_boundary(MUST(String::from_utf8(string)), [&](auto boundary) {
        boundaries.append(boundary);
        return IterationDecision::Continue;
    });

    EXPECT_EQ(boundaries, ReadonlySpan<size_t> { expected_boundaries });
}

TEST_CASE(line_segmentation)
{
    auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Line);

    segmenter->for_each_boundary(String {}, [&](auto) {
        VERIFY_NOT_REACHED();
        return IterationDecision::Break;
    });

    // Single characters.
    test_line_segmentation("a"sv, { 0u, 1u });

    // No break opportunities within a single word.
    test_line_segmentation("abc"sv, { 0u, 3u });

    // Break opportunity after whitespace.
    test_line_segmentation("ab cd"sv, { 0u, 3u, 5u });
    test_line_segmentation("ab  cd"sv, { 0u, 4u, 6u });
    test_line_segmentation("ab\tcd"sv, { 0u, 3u, 5u });

    // Hard line breaks.
    test_line_segmentation("ab\ncd"sv, { 0u, 3u, 5u });
    test_line_segmentation("ab\r\ncd"sv, { 0u, 4u, 6u });

    // CJK ideographs allow break between each character.
    test_line_segmentation("你好"sv, { 0u, 3u, 6u });
    test_line_segmentation("你好世界"sv, { 0u, 3u, 6u, 9u, 12u });

    // Mixed ASCII and CJK.
    test_line_segmentation("ab你好cd"sv, { 0u, 2u, 5u, 8u, 10u });
}

template<size_t N>
static void test_ascii_line_segmentation(Utf16String const& string, size_t const (&expected_boundaries)[N])
{
    auto segmenter = Unicode::Segmenter::try_create_for_ascii_line(string.utf16_view());
    VERIFY(segmenter);

    Vector<size_t> boundaries;
    segmenter->for_each_boundary(string.utf16_view(), [&](auto boundary) {
        boundaries.append(boundary);
        return IterationDecision::Continue;
    });

    EXPECT_EQ(boundaries, ReadonlySpan<size_t> { expected_boundaries });
}

static void expect_ascii_line_segmentation_matches_icu(Utf16String const& string, Unicode::Segmenter& icu_segmenter)
{
    auto ascii_segmenter = Unicode::Segmenter::try_create_for_ascii_line(string.utf16_view());
    VERIFY(ascii_segmenter);

    Vector<size_t> icu_boundaries;
    icu_segmenter.for_each_boundary(string.utf16_view(), [&](auto boundary) {
        icu_boundaries.append(boundary);
        return IterationDecision::Continue;
    });

    Vector<size_t> ascii_boundaries;
    ascii_segmenter->for_each_boundary(string.utf16_view(), [&](auto boundary) {
        ascii_boundaries.append(boundary);
        return IterationDecision::Continue;
    });

    EXPECT_EQ(ascii_boundaries, icu_boundaries);
}

TEST_CASE(ascii_line_segmentation)
{
    auto empty_segmenter = Unicode::Segmenter::try_create_for_ascii_line({});
    VERIFY(empty_segmenter);
    empty_segmenter->for_each_boundary(String {}, [&](auto) {
        VERIFY_NOT_REACHED();
        return IterationDecision::Break;
    });

    // Single characters and atomic words.
    test_ascii_line_segmentation("a"_utf16, { 0u, 1u });
    test_ascii_line_segmentation("abc"_utf16, { 0u, 3u });

    // Break opportunity after whitespace.
    test_ascii_line_segmentation("ab cd"_utf16, { 0u, 3u, 5u });
    test_ascii_line_segmentation("ab  cd"_utf16, { 0u, 4u, 6u });
    test_ascii_line_segmentation("ab\tcd"_utf16, { 0u, 3u, 5u });

    // Hard line breaks.
    test_ascii_line_segmentation("ab\ncd"_utf16, { 0u, 3u, 5u });
    test_ascii_line_segmentation("ab\r\ncd"_utf16, { 0u, 4u, 6u });
    test_ascii_line_segmentation("ab\rcd"_utf16, { 0u, 3u, 5u });

    // Alphanumerics are atomic across letter/digit boundaries.
    test_ascii_line_segmentation("abc123"_utf16, { 0u, 6u });
    test_ascii_line_segmentation("123abc"_utf16, { 0u, 6u });
    test_ascii_line_segmentation("a 1 b"_utf16, { 0u, 2u, 4u, 5u });

    // Printable ASCII punctuation follows UAX#14 line-breaking rules.
    test_ascii_line_segmentation("example.com"_utf16, { 0u, 11u });      // LB15d (× IS) and LB29 (IS × AL) keep the dotted name atomic.
    test_ascii_line_segmentation("hello, world"_utf16, { 0u, 7u, 12u }); // Break after the space (LB18).
    test_ascii_line_segmentation("a/b/c"_utf16, { 0u, 2u, 4u, 5u });     // LB13 × SY, default ÷ after SY.
    test_ascii_line_segmentation("http://a/b"_utf16, { 0u, 7u, 9u, 10u });
    test_ascii_line_segmentation("\"ab\""_utf16, { 0u, 4u });        // LB19 around QU.
    test_ascii_line_segmentation("$100"_utf16, { 0u, 4u });          // LB25 PR × NU and NU × NU.
    test_ascii_line_segmentation("5%"_utf16, { 0u, 2u });            // LB25 NU × PO.
    test_ascii_line_segmentation("a+$b"_utf16, { 0u, 2u, 4u });      // LB24 AL × PR; PR × PR has no rule.
    test_ascii_line_segmentation("x%20y"_utf16, { 0u, 5u });         // LB24 AL × PO; LB25 PO × NU; LB23 NU × AL.
    test_ascii_line_segmentation(" .23"_utf16, { 0u, 1u, 4u });      // LB15c forces a break before a leading decimal point.
    test_ascii_line_segmentation("1/2"_utf16, { 0u, 3u });           // LB25 NU × SY × NU.
    test_ascii_line_segmentation("1)$"_utf16, { 0u, 3u });           // LB25 NU × CP and CP × PR.
    test_ascii_line_segmentation("(ab)"_utf16, { 0u, 4u });          // LB14 OP × ; LB13 × CP.
    test_ascii_line_segmentation(") ("_utf16, { 0u, 2u, 3u });       // LB18 break after SP.
    test_ascii_line_segmentation("a(b)c"_utf16, { 0u, 5u });         // LB30 AL × OP and CP × AL.
    test_ascii_line_segmentation("a-\"b"_utf16, { 0u, 4u });         // LB21 × HY and LB19 around QU.
    test_ascii_line_segmentation("what?yes"_utf16, { 0u, 5u, 8u });  // LB13 × EX, default ÷ after EX.
    test_ascii_line_segmentation("what?\"yes\""_utf16, { 0u, 10u }); // LB19 keeps the quoted segment atomic.

    // U+007C VERTICAL LINE is UAX#14 class BA: no break before, break after.
    test_ascii_line_segmentation("a|b"_utf16, { 0u, 2u, 3u });
    test_ascii_line_segmentation("aaa|bbb"_utf16, { 0u, 4u, 7u });
    test_ascii_line_segmentation("a||b"_utf16, { 0u, 3u, 4u });

    // Hyphen behavior follows UAX#14: × HY before, ÷ after when not in a numeric expression.
    // LB20a additionally glues a leading hyphen to the following letter.
    test_ascii_line_segmentation("-foo"_utf16, { 0u, 4u });
    test_ascii_line_segmentation("x -foo"_utf16, { 0u, 2u, 6u });
    test_ascii_line_segmentation("word-break"_utf16, { 0u, 5u, 10u });
    // LB25 keeps numeric expressions atomic across hyphens (HY × NU).
    test_ascii_line_segmentation("-2"_utf16, { 0u, 2u });
    test_ascii_line_segmentation("foo-2"_utf16, { 0u, 5u });
    test_ascii_line_segmentation("ABCD-1234"_utf16, { 0u, 9u });
    test_ascii_line_segmentation("-#tag"_utf16, { 0u, 5u });
}

TEST_CASE(ascii_line_segmentation_matches_icu_for_context_independent_text)
{
    // The fast path implements UAX#14 for ASCII inputs and should agree with ICU on every input it accepts.
    Array test_strings = {
        "a"_utf16,
        "abc"_utf16,
        "ab cd"_utf16,
        "ab  cd"_utf16,
        "ab\tcd"_utf16,
        "ab\ncd"_utf16,
        "ab\r\ncd"_utf16,
        "abc123"_utf16,
        "a 1 b 2"_utf16,
        "The quick brown fox jumps over the lazy dog"_utf16,
        "example.com"_utf16,
        "hello, world"_utf16,
        "a/b/c"_utf16,
        "http://a/b"_utf16,
        "\"ab\""_utf16,
        "$100"_utf16,
        "5%"_utf16,
        "a+$b"_utf16,
        "x%20y"_utf16,
        " .23"_utf16,
        "1/2"_utf16,
        "1)$"_utf16,
        "(ab)"_utf16,
        ") ("_utf16,
        "a(b)c"_utf16,
        "a-\"b"_utf16,
        "what?yes"_utf16,
        "what?\"yes\""_utf16,
        "a|b"_utf16,
        "-foo"_utf16,
        "word-break"_utf16,
        "foo-2"_utf16,
        "ABCD-1234"_utf16,
        "a[b]c"_utf16,
        "$(100)"_utf16,
        "10/%"_utf16,
        "/0"_utf16,
        ")%"_utf16,
        "x -foo"_utf16,
        "\"[0-9]{8,19}\""_utf16,
    };

    auto icu_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Line);

    for (auto const& string : test_strings) {
        expect_ascii_line_segmentation_matches_icu(string, *icu_segmenter);
    }
}

TEST_CASE(try_create_for_ascii_line)
{
    auto eligible = [](StringView text) {
        auto string = Utf16String::from_utf8(text);
        return Unicode::Segmenter::try_create_for_ascii_line(string.utf16_view()) != nullptr;
    };

    // Letters, digits, and whitespace are admitted.
    EXPECT(eligible(""sv));
    EXPECT(eligible("hello"sv));
    EXPECT(eligible("hello world"sv));
    EXPECT(eligible("123"sv));
    EXPECT(eligible("abc 123\nxyz"sv));
    EXPECT(eligible("a\tb\r\nc"sv));

    // Printable ASCII punctuation is admitted.
    EXPECT(eligible("hello, world"sv));
    EXPECT(eligible("a/b"sv));
    EXPECT(eligible("example.com"sv));
    EXPECT(eligible("word-break"sv));
    EXPECT(eligible("$100"sv));
    EXPECT(eligible("\"quoted\""sv));
    EXPECT(eligible("1/2"sv));
    EXPECT(eligible(" .23"sv));
    EXPECT(eligible("-#tag"sv));
    EXPECT(eligible("(ab)"sv));
    EXPECT(eligible("[ab]"sv));
    EXPECT(eligible("{ab}"sv));

    // Unsupported controls and non-ASCII are rejected so that ICU handles them.
    EXPECT(!eligible("\x1b"sv));
    EXPECT(!eligible("你好"sv));
}

TEST_CASE(ascii_line_segmenter_clone_independence)
{
    auto utf16_a = "ab cd"_utf16;
    auto segmenter_a = Unicode::Segmenter::try_create_for_ascii_line(utf16_a.utf16_view());
    VERIFY(segmenter_a);

    auto segmenter_b = segmenter_a->clone();
    segmenter_b->set_segmented_text("xy"_string);

    EXPECT_EQ(segmenter_a->next_boundary(0).value_or(0u), 3u);
    EXPECT_EQ(segmenter_b->next_boundary(0).value_or(0u), 2u);
}

TEST_CASE(ascii_line_segmenter_next_boundary_inclusive)
{
    auto utf16 = "ab cd"_utf16;
    auto segmenter = Unicode::Segmenter::try_create_for_ascii_line(utf16.utf16_view());
    VERIFY(segmenter);

    // Inclusive::Yes returns the index itself when it is a boundary.
    EXPECT_EQ(segmenter->next_boundary(0, Unicode::Segmenter::Inclusive::Yes).value_or(0u), 0u);
    EXPECT_EQ(segmenter->next_boundary(3, Unicode::Segmenter::Inclusive::Yes).value_or(0u), 3u);
    EXPECT_EQ(segmenter->next_boundary(5, Unicode::Segmenter::Inclusive::Yes).value_or(0u), 5u);

    // Inclusive::Yes at a non-boundary returns the next boundary.
    EXPECT_EQ(segmenter->next_boundary(1, Unicode::Segmenter::Inclusive::Yes).value_or(0u), 3u);
    EXPECT_EQ(segmenter->next_boundary(2, Unicode::Segmenter::Inclusive::Yes).value_or(0u), 3u);
    EXPECT_EQ(segmenter->next_boundary(4, Unicode::Segmenter::Inclusive::Yes).value_or(0u), 5u);

    // Past the end returns empty.
    EXPECT(!segmenter->next_boundary(5).has_value());
    EXPECT(!segmenter->next_boundary(10).has_value());

    // Previous boundary clamps from past the end, matching the ICU-backed segmenter.
    EXPECT_EQ(segmenter->previous_boundary(10).value_or(0u), 5u);
}

TEST_CASE(out_of_bounds)
{
    {
        auto text = "foo"_string;

        auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Word);
        segmenter->set_segmented_text(text);

        auto result = segmenter->previous_boundary(text.byte_count() + 1);
        EXPECT(result.has_value());

        result = segmenter->next_boundary(text.byte_count() + 1);
        EXPECT(!result.has_value());

        result = segmenter->previous_boundary(text.byte_count());
        EXPECT(result.has_value());

        result = segmenter->next_boundary(text.byte_count());
        EXPECT(!result.has_value());

        result = segmenter->next_boundary(0);
        EXPECT(result.has_value());

        result = segmenter->previous_boundary(0);
        EXPECT(!result.has_value());
    }
    {
        auto text = u"foo"_utf16;

        auto segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Word);
        segmenter->set_segmented_text(text);

        auto result = segmenter->previous_boundary(text.length_in_code_units() + 1);
        EXPECT(result.has_value());

        result = segmenter->next_boundary(text.length_in_code_units() + 1);
        EXPECT(!result.has_value());

        result = segmenter->previous_boundary(text.length_in_code_units());
        EXPECT(result.has_value());

        result = segmenter->next_boundary(text.length_in_code_units());
        EXPECT(!result.has_value());

        result = segmenter->next_boundary(0);
        EXPECT(result.has_value());

        result = segmenter->previous_boundary(0);
        EXPECT(!result.has_value());
    }
}

static Vector<size_t> boundaries_without_icu_data(Unicode::SegmenterGranularity granularity, StringView string)
{
    Vector<size_t> boundaries;
    auto segmenter = Unicode::Segmenter::create_without_icu_data(granularity);
    segmenter->for_each_boundary(MUST(String::from_utf8(string)), [&](auto boundary) {
        boundaries.append(boundary);
        return IterationDecision::Continue;
    });
    return boundaries;
}

TEST_CASE(segmentation_without_icu_data)
{
    using Unicode::SegmenterGranularity;

    EXPECT(boundaries_without_icu_data(SegmenterGranularity::Grapheme, ""sv).is_empty());
    EXPECT_EQ(boundaries_without_icu_data(SegmenterGranularity::Grapheme, "a\xc3\xa9z"sv), (Vector<size_t> { 0, 1, 3, 4 }));
    EXPECT_EQ(boundaries_without_icu_data(SegmenterGranularity::Word, "hello, world"sv), (Vector<size_t> { 0, 5, 6, 7, 12 }));
    EXPECT_EQ(boundaries_without_icu_data(SegmenterGranularity::Word, "a  b"sv), (Vector<size_t> { 0, 1, 3, 4 }));
    EXPECT_EQ(boundaries_without_icu_data(SegmenterGranularity::Line, "hello, world"sv), (Vector<size_t> { 0, 7, 12 }));
    EXPECT_EQ(boundaries_without_icu_data(SegmenterGranularity::Sentence, "Hi. Bye."sv), (Vector<size_t> { 0, 8 }));

    auto grapheme = Unicode::Segmenter::create_without_icu_data(SegmenterGranularity::Grapheme);
    Vector<size_t> utf16_boundaries;
    grapheme->for_each_boundary(u"a\U0001F600b"_utf16, [&](auto boundary) {
        utf16_boundaries.append(boundary);
        return IterationDecision::Continue;
    });
    EXPECT_EQ(utf16_boundaries, (Vector<size_t> { 0, 1, 3, 4 }));

    auto word = Unicode::Segmenter::create_without_icu_data(SegmenterGranularity::Word);
    word->set_segmented_text("hello, world"_string);
    EXPECT_EQ(word->next_boundary(2), 5u);
    EXPECT(word->is_current_boundary_word_like());
    EXPECT_EQ(word->next_boundary(5), 6u);
    EXPECT(!word->is_current_boundary_word_like());
    EXPECT_EQ(word->next_boundary(5, Unicode::Segmenter::Inclusive::Yes), 5u);
    EXPECT_EQ(word->previous_boundary(6), 5u);
    EXPECT_EQ(word->previous_boundary(5, Unicode::Segmenter::Inclusive::Yes), 5u);
    EXPECT_EQ(word->previous_boundary(0, Unicode::Segmenter::Inclusive::Yes), 0u);
    EXPECT_EQ(word->previous_boundary(100), 7u);
    EXPECT_EQ(word->previous_boundary(0), OptionalNone {});
    EXPECT_EQ(word->next_boundary(12), OptionalNone {});
    EXPECT_EQ(word->next_boundary(100, Unicode::Segmenter::Inclusive::Yes), 12u);
    EXPECT_EQ(word->next_boundary(3, Unicode::Segmenter::Inclusive::Yes), 5u);
}
