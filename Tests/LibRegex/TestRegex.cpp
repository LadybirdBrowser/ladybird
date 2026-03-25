/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibRegex/ECMAScriptRegex.h>

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
