/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>

#include <LibRegex/ECMAScriptRegex.h>

static regex::ECMAScriptRegex compile_regex(StringView pattern, regex::ECMAScriptCompileFlags flags = {})
{
    return MUST(regex::ECMAScriptRegex::compile(pattern, flags));
}

static bool compile_succeeds(StringView pattern, regex::ECMAScriptCompileFlags flags = {})
{
    return !regex::ECMAScriptRegex::compile(pattern, flags).is_error();
}

static bool matches(StringView pattern, StringView subject, regex::ECMAScriptCompileFlags flags = {})
{
    auto regex = compile_regex(pattern, flags);
    auto utf16_subject = Utf16String::from_utf8(subject);
    auto result = regex.test(utf16_subject, 0);
    EXPECT(result != regex::MatchResult::LimitExceeded);
    return result == regex::MatchResult::Match;
}

static Optional<Utf16View> capture_group(regex::ECMAScriptRegex const& regex, Utf16View input, unsigned group_index)
{
    auto start = regex.capture_slot(group_index * 2);
    auto end = regex.capture_slot(group_index * 2 + 1);
    if (start < 0 || end < 0)
        return {};
    return input.substring_view(start, end - start);
}

static void expect_capture_eq(regex::ECMAScriptRegex const& regex, Utf16View input, unsigned group_index, StringView expected)
{
    auto capture = capture_group(regex, input, group_index);
    EXPECT(capture.has_value());
    if (capture.has_value())
        EXPECT(*capture == expected);
}

static void expect_capture_unmatched(regex::ECMAScriptRegex const& regex, unsigned group_index)
{
    EXPECT_EQ(regex.capture_slot(group_index * 2), -1);
    EXPECT_EQ(regex.capture_slot(group_index * 2 + 1), -1);
}

TEST_CASE(compile_rejects_invalid_pattern)
{
    auto regex = regex::ECMAScriptRegex::compile("("sv, {});
    EXPECT(regex.is_error());
}

TEST_CASE(exec_tracks_named_capture_slots)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("(?<word>foo)(bar)"sv, {}));

    EXPECT_EQ(regex.capture_count(), 2u);
    EXPECT_EQ(regex.total_groups(), 3u);
    EXPECT_EQ(regex.named_groups().size(), 1u);
    EXPECT_EQ(regex.named_groups()[0].name, "word"sv);
    EXPECT_EQ(regex.named_groups()[0].index, 1u);

    EXPECT_EQ(regex.exec(u"foobar"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.capture_slot(0), 0);
    EXPECT_EQ(regex.capture_slot(1), 6);
    EXPECT_EQ(regex.capture_slot(2), 0);
    EXPECT_EQ(regex.capture_slot(3), 3);
    EXPECT_EQ(regex.capture_slot(4), 3);
    EXPECT_EQ(regex.capture_slot(5), 6);
}

TEST_CASE(exec_reports_unmatched_optional_groups)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("(foo)?bar"sv, {}));

    EXPECT_EQ(regex.exec(u"bar"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.capture_slot(0), 0);
    EXPECT_EQ(regex.capture_slot(1), 3);
    EXPECT_EQ(regex.capture_slot(2), -1);
    EXPECT_EQ(regex.capture_slot(3), -1);
}

TEST_CASE(ascii_backed_inputs_preserve_match_results)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("(?<word>foo)(bar)"sv, {}));

    EXPECT_EQ(regex.exec("foobar"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.capture_slot(0), 0);
    EXPECT_EQ(regex.capture_slot(1), 6);
    EXPECT_EQ(regex.capture_slot(2), 0);
    EXPECT_EQ(regex.capture_slot(3), 3);
    EXPECT_EQ(regex.capture_slot(4), 3);
    EXPECT_EQ(regex.capture_slot(5), 6);

    EXPECT_EQ(regex.test("foobar"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.find_all("foobar foobar"sv, 0), 2);
    EXPECT_EQ(regex.find_all_match(0).start, 0);
    EXPECT_EQ(regex.find_all_match(0).end, 6);
    EXPECT_EQ(regex.find_all_match(1).start, 7);
    EXPECT_EQ(regex.find_all_match(1).end, 13);
}

TEST_CASE(test_honors_ignore_case)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("casesensitive"sv, { .ignore_case = true }));

    EXPECT_EQ(regex.test(u"CaseSensitive"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.test(u"something else"sv, 0), regex::MatchResult::NoMatch);
}

TEST_CASE(find_all_returns_non_overlapping_matches)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("aba"sv, {}));

    EXPECT_EQ(regex.find_all(u"aba aba"sv, 0), 2);
    EXPECT_EQ(regex.find_all_match(0).start, 0);
    EXPECT_EQ(regex.find_all_match(0).end, 3);
    EXPECT_EQ(regex.find_all_match(1).start, 4);
    EXPECT_EQ(regex.find_all_match(1).end, 7);
}

TEST_CASE(unicode_property_matching_works)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("\\p{ASCII}+"sv, { .unicode = true }));

    EXPECT_EQ(regex.test(u"ASCII"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.test(u"😀"sv, 0), regex::MatchResult::NoMatch);
}

TEST_CASE(end_anchored_suffix_patterns_preserve_behavior)
{
    auto regex = MUST(regex::ECMAScriptRegex::compile("(.*)\\/client-(.*)\\.js$"sv, {}));

    EXPECT_EQ(regex.test(u"https://cdn.example.com/assets/client-main.js"sv, 0), regex::MatchResult::Match);
    EXPECT_EQ(regex.test(u"<script src=\"/assets/client-main.js\"></script>"sv, 0), regex::MatchResult::NoMatch);
}

TEST_CASE(restored_ecmascript_parse_coverage)
{
    struct Test {
        StringView pattern;
        bool should_compile { true };
        regex::ECMAScriptCompileFlags flags {};
    };

    static constexpr Test tests[] {
        { "^hello.$"sv },
        { "\\x"sv },
        { "\\x1"sv },
        { "\\x1"sv, false, { .unicode = true } },
        { "\\x11"sv, true, { .unicode = true } },
        { "\\"sv, false },
        { "(?"sv, false },
        { "\\u1234"sv, true, { .unicode = true } },
        { "[\\u1234]"sv, true, { .unicode = true } },
        { "\\u1"sv, false, { .unicode = true } },
        { "[\\u1]"sv, false, { .unicode = true } },
        { "{1}"sv, false },
        { "{1,2}"sv, false },
        { "\\uxxxx"sv, false, { .unicode = true } },
        { "\\u{10ffff}"sv, true, { .unicode = true } },
        { "\\u{110000}"sv, false, { .unicode = true } },
        { "\\p{ASCII}"sv, true, { .unicode = true } },
        { "\\p{}"sv, false, { .unicode = true } },
        { "\\p{AsCiI}"sv, false, { .unicode = true } },
        { "(?<a>a)(?<a>b)"sv, false },
        { "(?:(?<x>a)|(?<y>a)(?<x>b))(?:(?<z>c)|(?<z>d))"sv },
        { "(?<1a>a)"sv, false },
        { "(?<$$_$$>a)"sv },
        { "(?<ÿ>a)"sv },
        { "(?<𝓑𝓻𝓸𝔀𝓷>a)"sv },
        { "(?ii:a)"sv, false },
        { "(?-:a)"sv, false },
        { "(?i)"sv, false },
        { "(?-i)"sv, false },
        { "["sv, false },
        { "[ -"sv, false },
        { "[[x[]]]"sv, true, { .unicode_sets = true } },
        { "[\\w--x]"sv, true, { .unicode_sets = true } },
    };

    for (auto const& test : tests)
        EXPECT_EQ(compile_succeeds(test.pattern, test.flags), test.should_compile);
}

TEST_CASE(restored_ecmascript_match_coverage)
{
    struct Test {
        StringView pattern;
        StringView subject;
        bool should_match { true };
        regex::ECMAScriptCompileFlags flags {};
    };

    static constexpr Test tests[] {
        { "^hello.$"sv, "hello1"sv },
        { "^h{0,1}ello.$"sv, "ello1"sv },
        { "^hell\\x6f1$"sv, "hello1"sv },
        { "^hel(?<LO>l.)1$"sv, "hello1"sv },
        { "\\b.*\\b"sv, "hello1"sv },
        { "bar(?=f.)foo"sv, "barfoo"sv },
        { "bar(?=foo)bar"sv, "barbar"sv, false },
        { "bar(?!foo)bar"sv, "barbar"sv },
        { "bar(?!bar)bar"sv, "barbar"sv, false },
        { "bar.*(?<=foo)"sv, "barbar"sv, false },
        { "bar.*(?<!foo)"sv, "barbar"sv },
        { "(?:)"sv, ""sv },
        { "(?<=.{3})f"sv, "abcdef"sv },
        { "(?<=.{3})f"sv, "abc😀ef"sv, true, { .unicode = true } },
        { "a(?=.(?=c)|b)b"sv, "ab"sv },
        { "(?=)(?=\\d)"sv, "smart"sv, false },
        { "(?<!.*q.*?)(?<=h.*)THIS(?=.*!)"sv, "hey THIS does match!"sv },
        { "(.*a)?(x)"sv, "x"sv },
        { "^\\w*[\\u212A]"sv, "K"sv, true, { .ignore_case = true, .unicode = true } },
        { "^a*A\\d"sv, "aaaa5"sv, true, { .ignore_case = true } },
        { "^\\u{017f}*s$"sv, "ſs"sv, true, { .ignore_case = true, .unicode = true } },
        { "(a+)+b"sv, "aaaaaaaaaaaaaaaaaaaaaaaaa"sv, false },
    };

    for (auto const& test : tests)
        EXPECT_EQ(matches(test.pattern, test.subject, test.flags), test.should_match);
}

TEST_CASE(restored_lookbehind_capture_coverage)
{
    {
        auto regex = compile_regex("(?<=(a|cc))b"sv);
        auto subject = Utf16String::from_utf8("ccb"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "b"sv);
        expect_capture_eq(regex, subject, 1, "cc"sv);
    }
    {
        auto regex = compile_regex("((?<=\\b)[d-f]{3})"sv);
        auto subject = Utf16String::from_utf8("abc def"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "def"sv);
        expect_capture_eq(regex, subject, 1, "def"sv);
    }
    {
        auto regex = compile_regex("(?<=(b+))c"sv);
        auto subject = Utf16String::from_utf8("abbbbbbc"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "c"sv);
        expect_capture_eq(regex, subject, 1, "bbbbbb"sv);
    }
    {
        auto regex = compile_regex("(?<=((?:b\\d{2})+))c"sv);
        auto subject = Utf16String::from_utf8("ab12b23b34c"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "c"sv);
        expect_capture_eq(regex, subject, 1, "b12b23b34"sv);
    }
}

TEST_CASE(restored_inversion_state_in_char_class_coverage)
{
    {
        auto regex = compile_regex("[\\S\\s]"sv);
        auto subject = Utf16String::from_utf8("hello"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "h"sv);
    }
    {
        auto regex = compile_regex("[^\\S\\n]"sv);
        auto subject = Utf16String::from_utf8("\n"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::NoMatch);
    }
    {
        auto regex = compile_regex("[^\\S]"sv);
        auto subject = Utf16String::from_utf8("\t"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "\t"sv);
    }
}

TEST_CASE(restored_quantified_alternation_capture_coverage)
{
    {
        auto regex = compile_regex("^(a|a?)+$"sv);
        auto subject = Utf16String::from_utf8("a"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "a"sv);
        expect_capture_eq(regex, subject, 1, "a"sv);
    }
    {
        auto regex = compile_regex("^(a|a?)+$"sv);
        auto subject = Utf16String::from_utf8("aa"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "aa"sv);
        expect_capture_eq(regex, subject, 1, "a"sv);
    }
}

TEST_CASE(restored_zero_width_backreference_coverage)
{
    {
        auto regex = compile_regex("(a*)b\\1+"sv);
        auto subject = Utf16String::from_utf8("baaac"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "b"sv);
        expect_capture_eq(regex, subject, 1, ""sv);
    }
    {
        auto regex = compile_regex("(x)?\\1y"sv);
        auto subject = Utf16String::from_utf8("y"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "y"sv);
        expect_capture_unmatched(regex, 1);
    }
    {
        auto regex = compile_regex("(?!(y)y)(\\1)z"sv);
        auto subject = Utf16String::from_utf8("xyyz"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "z"sv);
        expect_capture_unmatched(regex, 1);
        expect_capture_eq(regex, subject, 2, ""sv);
    }
}

TEST_CASE(restored_backreference_to_undefined_capture_groups)
{
    {
        auto regex = compile_regex("(?:(?<x>a)|(?<x>b))\\k<x>"sv);
        auto subject = Utf16String::from_utf8("bb"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "bb"sv);
        expect_capture_unmatched(regex, 1);
        expect_capture_eq(regex, subject, 2, "b"sv);
    }
    {
        auto regex = compile_regex("(?:(?:(?<x>a)|(?<x>b))\\k<x>){2}"sv);
        auto subject = Utf16String::from_utf8("aabb"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "aabb"sv);
        expect_capture_unmatched(regex, 1);
        expect_capture_eq(regex, subject, 2, "b"sv);
    }
    {
        auto regex = compile_regex("(?:(?<x>a)|(?<x>b))\\k<x>"sv);
        auto subject = Utf16String::from_utf8("aa"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "aa"sv);
        expect_capture_eq(regex, subject, 1, "a"sv);
        expect_capture_unmatched(regex, 2);
    }
    {
        auto regex = compile_regex("(.*?)a(?!(a+)b\\2c)\\2(.*)"sv);
        auto subject = Utf16String::from_utf8("baaabaac"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "baaabaac"sv);
        expect_capture_eq(regex, subject, 1, "ba"sv);
        expect_capture_unmatched(regex, 2);
        expect_capture_eq(regex, subject, 3, "abaac"sv);
    }
    {
        auto regex = compile_regex("^(?:(?<a>x)|(?<a>y)|z)\\k<a>$"sv);
        auto subject = Utf16String::from_utf8("z"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "z"sv);
        expect_capture_unmatched(regex, 1);
        expect_capture_unmatched(regex, 2);
    }
    {
        auto regex = compile_regex("^(?:(?<a>x)|(?<a>y)|z){2}\\k<a>$"sv);
        auto subject = Utf16String::from_utf8("xz"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "xz"sv);
        expect_capture_unmatched(regex, 1);
        expect_capture_unmatched(regex, 2);
    }
}

TEST_CASE(restored_optional_groups_with_empty_matches)
{
    {
        auto regex = compile_regex("^(.*)(.*)?$"sv);
        auto subject = Utf16String::from_utf8("a"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 1, "a"sv);
        expect_capture_unmatched(regex, 2);
    }
    {
        auto regex = compile_regex("()?"sv);
        auto subject = Utf16String::from_utf8(""sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_unmatched(regex, 1);
    }
    {
        auto regex = compile_regex("(z)((a+)?(b+)?(c))*"sv);
        auto subject = Utf16String::from_utf8("zaacbbbcac"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 1, "z"sv);
        expect_capture_eq(regex, subject, 2, "ac"sv);
        expect_capture_eq(regex, subject, 3, "a"sv);
        expect_capture_unmatched(regex, 4);
        expect_capture_eq(regex, subject, 5, "c"sv);
    }
    {
        auto regex = compile_regex("(?:(?=(abc)))?a"sv);
        auto subject = Utf16String::from_utf8("abc"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "a"sv);
        expect_capture_unmatched(regex, 1);
    }
    {
        auto regex = compile_regex("^(?:(?=(abc))){0,1}a"sv);
        auto subject = Utf16String::from_utf8("abc"sv);

        EXPECT_EQ(regex.exec(subject, 0), regex::MatchResult::Match);
        expect_capture_eq(regex, subject, 0, "a"sv);
        expect_capture_unmatched(regex, 1);
    }
}

TEST_CASE(restored_ecmascript_modifier_coverage)
{
    struct Test {
        StringView pattern;
        StringView subject;
        bool should_match { true };
        regex::ECMAScriptCompileFlags flags {};
    };

    static constexpr Test tests[] {
        { "a(?i:b)c"sv, "aBc"sv },
        { "a(?i:b)c"sv, "aBC"sv, false },
        { "a(?s:.)c"sv, "a\nc"sv },
        { "(?ims:a.b)"sv, "A\nB"sv },
        { "(?i:a(?-i:b)c)"sv, "AbC"sv },
        { "(?i:a(?-i:b)c)"sv, "ABC"sv, false },
        { "a(?-i:b)c"sv, "AbC"sv, true, { .ignore_case = true } },
        { "a(?-i:b)c"sv, "ABC"sv, false, { .ignore_case = true } },
        { "x.(?m:^a)"sv, "x\na"sv, true, { .dot_all = true } },
    };

    for (auto const& test : tests)
        EXPECT_EQ(matches(test.pattern, test.subject, test.flags), test.should_match);
}

TEST_CASE(restored_unicode_property_and_sets_coverage)
{
    struct Test {
        StringView pattern;
        StringView subject;
        bool should_match { true };
        regex::ECMAScriptCompileFlags flags {};
    };

    static constexpr Test tests[] {
        { "\\p{ASCII}"sv, "a"sv, false },
        { "\\p{ASCII}"sv, "p{ASCII}"sv },
        { "\\p{ASCII}"sv, "a"sv, true, { .unicode = true } },
        { "\\p{ASCII}"sv, "😀"sv, false, { .unicode = true } },
        { "\\P{ASCII}"sv, "a"sv, false, { .unicode = true } },
        { "\\P{ASCII}"sv, "😀"sv, true, { .unicode = true } },
        { "\\p{ASCII_Hex_Digit}"sv, "1"sv, true, { .unicode = true } },
        { "\\P{ASCII_Hex_Digit}"sv, "x"sv, true, { .unicode = true } },
        { "\\p{General_Category=Cased_Letter}"sv, "A"sv, true, { .unicode = true } },
        { "\\P{Cased_Letter}"sv, "9"sv, true, { .unicode = true } },
        { "\\p{sc=Latin}"sv, "A"sv, true, { .unicode = true } },
        { "\\u{1f600}"sv, "😀"sv, true, { .unicode = true } },
        { "[\\w--x]"sv, "x"sv, false, { .unicode_sets = true } },
        { "[\\w--x]"sv, "y"sv, true, { .unicode_sets = true } },
        { "[\\w&&x]"sv, "x"sv, true, { .unicode_sets = true } },
        { "[[0-9\\w]--x--6]"sv, "6"sv, false, { .unicode_sets = true } },
        { "[[0-9\\w]--x--6]"sv, "9"sv, true, { .unicode_sets = true } },
    };

    for (auto const& test : tests)
        EXPECT_EQ(matches(test.pattern, test.subject, test.flags), test.should_match);
}

TEST_CASE(restored_empty_match_and_loop_coverage)
{
    static constexpr StringView patterns[] {
        "(a*)*"sv,
        "(a*?)*"sv,
        "(a*)*?"sv,
        "(?:)*?"sv,
        "(a?)+$"sv,
    };

    for (auto pattern : patterns)
        EXPECT(matches(pattern, ""sv));

    auto regex = compile_regex(".*"sv, { .global = true });
    auto subject = Utf16String::from_utf8(""sv);
    EXPECT_EQ(regex.find_all(subject, 0), 1);
    EXPECT_EQ(regex.find_all_match(0).start, 0);
    EXPECT_EQ(regex.find_all_match(0).end, 0);
}

TEST_CASE(restored_long_fork_chain_coverage)
{
    auto regex = compile_regex("(?:aa)*"sv);
    auto subject = MUST(String::repeated('a', 1000));
    auto utf16_subject = Utf16String::from_utf8(subject.bytes_as_string_view());

    EXPECT_EQ(regex.test(utf16_subject, 0), regex::MatchResult::Match);
}
