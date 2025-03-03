/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h> // import first, to prevent warning of VERIFY* redefinition

#include <LibRegex/RegexPattern.h>

TEST_CASE(basic)
{
    class RegexPatternExtension : public RegexPattern {
        static_assert(RegexPattern::Disjunction < RegexPattern::Anchor);
        static_assert(RegexPattern::Anchor < RegexPattern::Concatentation);
        static_assert(RegexPattern::Concatentation < RegexPattern::Character);
    };

    String const hello { "hello"_string };
    RegexPattern p1 { hello };

    EXPECT_EQ(p1.string(), hello);
    EXPECT_EQ(p1.precedence(), RegexPattern::Character); // default precedence

    for (auto const& prec : {
             RegexPattern::Disjunction,
             RegexPattern::Anchor,
             RegexPattern::Concatentation,
             RegexPattern::Character }) {
        RegexPattern const p { hello, prec };
        EXPECT_EQ(p.precedence(), prec);
        EXPECT_EQ(p.string(), hello);
    }
}

TEST_CASE(match) // Proves that the underlying Regex matches as expected.
{
    RegexPattern pattern { "([Hh][A-Za-z]*)[A-Za-z] [Ww](?<capture>[A-Za-z]*)"_string };
    String input = "hello World"_string;
    auto result = pattern.match(input);

    EXPECT(result.success);
    EXPECT_EQ(result.matches.size(), 1U);
    EXPECT_EQ(result.matches.at(0).view.string_view(), input);
    EXPECT_EQ(result.capture_group_matches.size(), 1U);
    EXPECT_EQ(result.capture_group_matches.at(0).size(), 2U);
    EXPECT_EQ(result.capture_group_matches.at(0).at(0).view.string_view(), "hell"_string);
    EXPECT_EQ(result.capture_group_matches.at(0).at(1).capture_group_name, "capture"sv);
    EXPECT_EQ(result.capture_group_matches.at(0).at(1).view.string_view(), "orld"sv);
}

TEST_CASE(group)
{
    RegexPattern pattern { "hello"_string };

    EXPECT_EQ(pattern.group('A').string(), "(?<A>hello)"_string);
    EXPECT_EQ(pattern.group('A').precedence(), RegexPattern::Character);

    EXPECT_EQ(pattern.group("world"_string).string(), "(?<world>hello)"_string);
    EXPECT_EQ(pattern.group("world"_string).precedence(), RegexPattern::Character);

    EXPECT_EQ(pattern.group().string(), "(?:hello)"_string);
    EXPECT_EQ(pattern.group().precedence(), RegexPattern::Character);
}

TEST_CASE(concatenation)
{
    String const hello { "hello"_string };
    String const world { "world"_string };

    struct Operation {
        RegexPattern::Precedence prec1;
        RegexPattern::Precedence prec2;
        String result;
    };

    Operation const ops[] = {
        Operation { RegexPattern::Character, RegexPattern::Character, "helloworld"_string },
        Operation { RegexPattern::Concatentation, RegexPattern::Concatentation, "helloworld"_string },
        Operation { RegexPattern::Concatentation, RegexPattern::Character, "helloworld"_string },
        Operation { RegexPattern::Concatentation, RegexPattern::Concatentation, "helloworld"_string },

        Operation { RegexPattern::Character, RegexPattern::Anchor, "hello(?:world)"_string },
        Operation { RegexPattern::Anchor, RegexPattern::Character, "(?:hello)world"_string },
        Operation { RegexPattern::Character, RegexPattern::Anchor, "hello(?:world)"_string },
        Operation { RegexPattern::Disjunction, RegexPattern::Disjunction, "(?:hello)(?:world)"_string },
    };

    for (auto const& op : ops) {
        RegexPattern p1 { hello, op.prec1 };
        RegexPattern p2 { world, op.prec2 };
        EXPECT_EQ((p1 + p2).string(), op.result);
        EXPECT_EQ((p1 + p2).precedence(), RegexPattern::Concatentation);
    }
}

TEST_CASE(one_of)
{
    RegexPattern one { "1"_string };
    RegexPattern two { "2"_string };
    RegexPattern three { "3"_string };
    RegexPattern four { "4"_string };

    EXPECT_EQ(one_of(one).string(), "1"_string);

    EXPECT_EQ(one_of(one, two).string(), "1|2"_string);
    EXPECT_EQ(one_of(one, two, three).string(), "1|2|3"_string);
    EXPECT_EQ(one_of(one, two, three, four).string(), "1|2|3|4"_string);
}

TEST_CASE(repetion)
{
    String const az { "[Az]"_string };
    RegexPattern p1 { az, RegexPattern::Character };
    RegexPattern p2 { az, RegexPattern::Concatentation };

    EXPECT_EQ(RegexPattern::maybe(p1).string(), "[Az]?"_string);
    EXPECT_EQ(RegexPattern::maybe(p1).precedence(), RegexPattern::Character);
    EXPECT_EQ(RegexPattern::maybe(p2).string(), "(?:[Az])?"_string);
    EXPECT_EQ(RegexPattern::maybe(p2).precedence(), RegexPattern::Character);

    EXPECT_EQ(p1.star().string(), "[Az]*"_string);
    EXPECT_EQ(p1.star().precedence(), RegexPattern::Character);
    EXPECT_EQ(p2.star().string(), "(?:[Az])*"_string);
    EXPECT_EQ(p2.star().precedence(), RegexPattern::Character);

    EXPECT_EQ(p1.plus().string(), "[Az]+"_string);
    EXPECT_EQ(p1.plus().precedence(), RegexPattern::Character);
    EXPECT_EQ(p2.plus().string(), "(?:[Az])+"_string);
    EXPECT_EQ(p2.plus().precedence(), RegexPattern::Character);

    EXPECT_EQ(p1.repeat(42).string(), "[Az]{42}"_string);
    EXPECT_EQ(p1.repeat(42).precedence(), RegexPattern::Character);
    EXPECT_EQ(p2.repeat(42).string(), "(?:[Az]){42}"_string);
    EXPECT_EQ(p2.repeat(42).precedence(), RegexPattern::Character);

    EXPECT_EQ(p1.repeat(16, 42).string(), "[Az]{16,42}"_string);
    EXPECT_EQ(p1.repeat(16, 42).precedence(), RegexPattern::Character);
    EXPECT_EQ(p2.repeat(16, 42).string(), "(?:[Az]){16,42}"_string);
    EXPECT_EQ(p2.repeat(16, 42).precedence(), RegexPattern::Character);
}

TEST_CASE(anchor)
{
    String const hello { "hello"_string };
    RegexPattern p1 { hello, RegexPattern::Character };
    RegexPattern p2 { hello, RegexPattern::Anchor };
    RegexPattern p3 { hello, RegexPattern::Disjunction };

    EXPECT_EQ(p1.first().string(), "^hello"_string);
    EXPECT_EQ(p1.first().precedence(), RegexPattern::Anchor);
    EXPECT_EQ(p2.first().string(), "^hello"_string);
    EXPECT_EQ(p2.first().precedence(), RegexPattern::Anchor);
    EXPECT_EQ(p3.first().string(), "^(?:hello)"_string);
    EXPECT_EQ(p3.first().precedence(), RegexPattern::Anchor);

    EXPECT_EQ(p1.last().string(), "hello$"_string);
    EXPECT_EQ(p1.last().precedence(), RegexPattern::Anchor);
    EXPECT_EQ(p2.last().string(), "hello$"_string);
    EXPECT_EQ(p2.last().precedence(), RegexPattern::Anchor);
    EXPECT_EQ(p3.last().string(), "(?:hello)$"_string);
    EXPECT_EQ(p3.last().precedence(), RegexPattern::Anchor);

    EXPECT_EQ(full(p1).string(), "^hello$"_string);
    EXPECT_EQ(full(p1).precedence(), RegexPattern::Anchor);
    EXPECT_EQ(full(p2).string(), "^hello$"_string);
    EXPECT_EQ(full(p2).precedence(), RegexPattern::Anchor);
    EXPECT_EQ(full(p3).string(), "^(?:hello)$"_string);
    EXPECT_EQ(full(p3).precedence(), RegexPattern::Anchor);
}

TEST_CASE(equal)
{
    RegexPattern p1 { "hello"_string };
    RegexPattern p2 { "hello"_string };
    RegexPattern p3 { "world"_string };

    EXPECT_EQ(p1, p2);
    EXPECT_NE(p1, p3);
}

TEST_CASE(capture)
{
    RegexPattern const pattern { "^(?<F>([+/-]*)(?<N>[0-9]*))$"_string };

    // Match. Empty groups.
    auto const r0 = pattern.match(""sv);

    EXPECT(r0.success);
    EXPECT_EQ(r0.matches.size(), 1U);
    EXPECT_EQ(r0.matches.at(0).view.string_view(), ""sv);

    EXPECT_EQ(r0.capture_group_matches.size(), 1U);
    EXPECT_EQ(r0.capture_group_matches.at(0).size(), 3U);

    auto const g01 = r0.capture_group_matches.at(0).at(0);
    auto const g02 = r0.capture_group_matches.at(0).at(1);
    auto const g03 = r0.capture_group_matches.at(0).at(2);

    EXPECT_EQ(g01.view.string_view(), ""sv);
    EXPECT_EQ(g02.view.string_view(), ""sv);
    EXPECT_EQ(g02.view.string_view(), ""sv);

    EXPECT_EQ(RegexPattern::group_name_first_char(g01), 'F');
    EXPECT(!RegexPattern::does_group_match_char(g01, 'A'));

    EXPECT(!RegexPattern::group_name_first_char(g02).has_value());

    EXPECT(RegexPattern::group_name_first_char(g03).has_value());
    EXPECT_EQ(RegexPattern::group_name_first_char(g03), 'N');
    EXPECT(!RegexPattern::does_group_match_char(g03, 'A'));

    // Match. Groups are not empty.
    auto const r1 = pattern.match("-123"sv);

    EXPECT(r1.success);
    EXPECT_EQ(r1.matches.size(), 1U);

    EXPECT_EQ(r1.matches.at(0).view.string_view(), "-123"sv);

    EXPECT_EQ(r1.capture_group_matches.size(), 1U);
    EXPECT_EQ(r1.capture_group_matches.at(0).size(), 3U);

    auto const g11 = r1.capture_group_matches.at(0).at(0);
    auto const g12 = r1.capture_group_matches.at(0).at(1);
    auto const g13 = r1.capture_group_matches.at(0).at(2);

    EXPECT_EQ(g11.view.string_view(), "-123"sv);
    EXPECT_EQ(g12.view.string_view(), "-"sv);
    EXPECT_EQ(g13.view.string_view(), "123"sv);

    EXPECT_EQ(RegexPattern::group_name_first_char(g11), 'F');
    EXPECT(!RegexPattern::does_group_match_char(g11, 'A'));

    EXPECT(!RegexPattern::group_name_first_char(g12).has_value());

    EXPECT(RegexPattern::group_name_first_char(g13).has_value());
    EXPECT_EQ(RegexPattern::group_name_first_char(g13), 'N');
    EXPECT(!RegexPattern::does_group_match_char(g13, 'A'));

    EXPECT(!RegexPattern::number<u64>(g11).has_value()); // '-123' cannot be converted to unsigned
    EXPECT_EQ(RegexPattern::number<i64>(g11), -123L);
    EXPECT(!RegexPattern::number<u64>(g12).has_value()); // '-' cannot be converted to number
    EXPECT_EQ(RegexPattern::number<u64>(g13), 123UL);

    EXPECT(!RegexPattern::string_to_decimals<u64>("123"_string, 0).has_value());
    EXPECT_EQ(RegexPattern::string_to_decimals<u64>("123"_string, 2), 12UL);
    EXPECT_EQ(RegexPattern::string_to_decimals<u64>("123"_string, 3), 123UL);
    EXPECT_EQ(RegexPattern::string_to_decimals<u64>("123"_string, 4), 1230UL);
    EXPECT_EQ(RegexPattern::string_to_decimals<u64>("123"_string, 9), 123000000UL);
}
