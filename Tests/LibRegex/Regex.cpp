/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h> // import first, to prevent warning of VERIFY* redefinition

#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <AK/Tuple.h>
#include <LibRegex/Regex.h>
#include <LibRegex/RegexDebug.h>
#include <LibRegex/RegexMatcher.h>
#include <stdio.h>

static ECMAScriptOptions match_test_api_options(ECMAScriptOptions const options)
{
    return options;
}

static PosixOptions match_test_api_options(PosixOptions const options)
{
    return options;
}

template<typename... Flags>
static constexpr ECMAScriptFlags combine_flags(Flags&&... flags)
requires((IsSame<Flags, ECMAScriptFlags> && ...))
{
    return static_cast<ECMAScriptFlags>((static_cast<regex::FlagsUnderlyingType>(flags) | ...));
}

TEST_CASE(regex_options_ecmascript)
{
    ECMAScriptOptions eo;
    eo |= ECMAScriptFlags::Global;

    EXPECT(eo.has_flag_set(ECMAScriptFlags::Global));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Insensitive));

    eo = match_test_api_options(ECMAScriptFlags::Global | ECMAScriptFlags::Insensitive | ECMAScriptFlags::Sticky);
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Global));
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Insensitive));
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Sticky));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Unicode));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Multiline));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::SingleLine));

    eo &= ECMAScriptFlags::Insensitive;
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Global));
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Insensitive));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Multiline));

    eo &= ECMAScriptFlags::Sticky;
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Global));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Insensitive));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Multiline));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Sticky));

    eo = ~ECMAScriptFlags::Insensitive;
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Global));
    EXPECT(!eo.has_flag_set(ECMAScriptFlags::Insensitive));
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Multiline));
    EXPECT(eo.has_flag_set(ECMAScriptFlags::Sticky));
}

TEST_CASE(regex_options_posix)
{
    PosixOptions eo;
    eo |= PosixFlags::Global;

    EXPECT(eo.has_flag_set(PosixFlags::Global));
    EXPECT(!eo.has_flag_set(PosixFlags::Insensitive));

    eo = match_test_api_options(PosixFlags::Global | PosixFlags::Insensitive | PosixFlags::MatchNotBeginOfLine);
    EXPECT(eo.has_flag_set(PosixFlags::Global));
    EXPECT(eo.has_flag_set(PosixFlags::Insensitive));
    EXPECT(eo.has_flag_set(PosixFlags::MatchNotBeginOfLine));
    EXPECT(!eo.has_flag_set(PosixFlags::Unicode));
    EXPECT(!eo.has_flag_set(PosixFlags::Multiline));

    eo &= PosixFlags::Insensitive;
    EXPECT(!eo.has_flag_set(PosixFlags::Global));
    EXPECT(eo.has_flag_set(PosixFlags::Insensitive));
    EXPECT(!eo.has_flag_set(PosixFlags::Multiline));

    eo &= PosixFlags::MatchNotBeginOfLine;
    EXPECT(!eo.has_flag_set(PosixFlags::Global));
    EXPECT(!eo.has_flag_set(PosixFlags::Insensitive));
    EXPECT(!eo.has_flag_set(PosixFlags::Multiline));

    eo = ~PosixFlags::Insensitive;
    EXPECT(eo.has_flag_set(PosixFlags::Global));
    EXPECT(!eo.has_flag_set(PosixFlags::Insensitive));
    EXPECT(eo.has_flag_set(PosixFlags::Multiline));
}

TEST_CASE(regex_lexer)
{
    Lexer l("/[.*+?^${}()|[\\]\\\\]/g"sv);
    EXPECT(l.next().type() == regex::TokenType::Slash);
    EXPECT(l.next().type() == regex::TokenType::LeftBracket);
    EXPECT(l.next().type() == regex::TokenType::Period);
    EXPECT(l.next().type() == regex::TokenType::Asterisk);
    EXPECT(l.next().type() == regex::TokenType::Plus);
    EXPECT(l.next().type() == regex::TokenType::Questionmark);
    EXPECT(l.next().type() == regex::TokenType::Circumflex);
    EXPECT(l.next().type() == regex::TokenType::Dollar);
    EXPECT(l.next().type() == regex::TokenType::LeftCurly);
    EXPECT(l.next().type() == regex::TokenType::RightCurly);
    EXPECT(l.next().type() == regex::TokenType::LeftParen);
    EXPECT(l.next().type() == regex::TokenType::RightParen);
    EXPECT(l.next().type() == regex::TokenType::Pipe);
    EXPECT(l.next().type() == regex::TokenType::LeftBracket);
    EXPECT(l.next().type() == regex::TokenType::EscapeSequence);
    EXPECT(l.next().type() == regex::TokenType::EscapeSequence);
    EXPECT(l.next().type() == regex::TokenType::RightBracket);
    EXPECT(l.next().type() == regex::TokenType::Slash);
    EXPECT(l.next().type() == regex::TokenType::Char);
}

TEST_CASE(parser_error_parens)
{
    ByteString pattern = "test()test";
    Lexer l(pattern);
    PosixExtendedParser p(l);
    p.parse();
    EXPECT(p.has_error());
    EXPECT(p.error() == regex::Error::EmptySubExpression);
}

TEST_CASE(parser_error_special_characters_used_at_wrong_place)
{
    ByteString pattern;
    Vector<char, 5> chars = { '*', '+', '?', '{' };
    StringBuilder b;

    Lexer l;
    PosixExtended p(l);

    for (auto& ch : chars) {
        // First in ere
        b.clear();
        b.append(ch);
        pattern = b.to_byte_string();
        l.set_source(pattern);
        p.parse();
        EXPECT(p.has_error());
        EXPECT(p.error() == regex::Error::InvalidRepetitionMarker);

        // After vertical line
        b.clear();
        b.append("a|"sv);
        b.append(ch);
        pattern = b.to_byte_string();
        l.set_source(pattern);
        p.parse();
        EXPECT(p.has_error());
        EXPECT(p.error() == regex::Error::InvalidRepetitionMarker);

        // After circumflex
        b.clear();
        b.append('^');
        b.append(ch);
        pattern = b.to_byte_string();
        l.set_source(pattern);
        p.parse();
        EXPECT(p.has_error());
        EXPECT(p.error() == regex::Error::InvalidRepetitionMarker);

        // After dollar
        b.clear();
        b.append('$');
        b.append(ch);
        pattern = b.to_byte_string();
        l.set_source(pattern);
        p.parse();
        EXPECT(p.has_error());
        EXPECT(p.error() == regex::Error::InvalidRepetitionMarker);

        // After left parens
        b.clear();
        b.append('(');
        b.append(ch);
        b.append(')');
        pattern = b.to_byte_string();
        l.set_source(pattern);
        p.parse();
        EXPECT(p.has_error());
        EXPECT(p.error() == regex::Error::InvalidRepetitionMarker);
    }
}

TEST_CASE(parser_error_vertical_line_used_at_wrong_place)
{
    Lexer l;
    PosixExtended p(l);

    // First in ere
    l.set_source("|asdf"sv);
    p.parse();
    EXPECT(p.has_error());
    EXPECT(p.error() == regex::Error::EmptySubExpression);

    // Last in ere
    l.set_source("asdf|"sv);
    p.parse();
    EXPECT(p.has_error());
    EXPECT(p.error() == regex::Error::EmptySubExpression);

    // After left parens
    l.set_source("(|asdf)"sv);
    p.parse();
    EXPECT(p.has_error());
    EXPECT(p.error() == regex::Error::EmptySubExpression);

    // Proceed right parens
    l.set_source("(asdf)|"sv);
    p.parse();
    EXPECT(p.has_error());
    EXPECT(p.error() == regex::Error::EmptySubExpression);
}

TEST_CASE(catch_all_first)
{
    Regex<PosixExtended> re("^.*$");
    RegexResult m;
    re.match("Hello World"sv, m);
    EXPECT(m.count == 1);
    EXPECT(re.match("Hello World"sv, m));
}

TEST_CASE(catch_all)
{
    Regex<PosixExtended> re("^.*$", PosixFlags::Global);

    EXPECT(re.has_match("Hello World"sv));
    EXPECT(re.match("Hello World"sv).success);
    EXPECT(re.match("Hello World"sv).count == 1);

    EXPECT(has_match("Hello World"sv, re));
    auto res = match("Hello World"sv, re);
    EXPECT(res.success);
    EXPECT(res.count == 1);
    EXPECT(res.matches.size() == 1);
    EXPECT(res.matches.first().view == "Hello World");
}

TEST_CASE(catch_all_again)
{
    Regex<PosixExtended> re("^.*$", PosixFlags::Extra);
    EXPECT_EQ(has_match("Hello World"sv, re), true);
}

TEST_CASE(char_utf8)
{
    Regex<PosixExtended> re("😀");
    RegexResult result;

    EXPECT_EQ((result = match(Utf8View { "Привет, мир! 😀 γειά σου κόσμος 😀 こんにちは世界"sv }, re, PosixFlags::Global)).success, true);
    EXPECT_EQ(result.count, 2u);
}

TEST_CASE(catch_all_newline)
{
    Regex<PosixExtended> re("^.*$", PosixFlags::Multiline);
    RegexResult result;
    ByteString aaa = "Hello World\nTest\n1234\n";
    auto lambda = [&]() {
        result = match(aaa, re);
        EXPECT_EQ(result.success, true);
    };
    lambda();
    EXPECT_EQ(result.count, 3u);
    EXPECT_EQ(result.matches.at(0).view, "Hello World");
    EXPECT_EQ(result.matches.at(1).view, "Test");
    EXPECT_EQ(result.matches.at(2).view, "1234");
}

TEST_CASE(catch_all_newline_view)
{
    Regex<PosixExtended> re("^.*$", PosixFlags::Multiline);
    RegexResult result;

    ByteString aaa = "Hello World\nTest\n1234\n";
    result = match(aaa, re);
    EXPECT_EQ(result.success, true);
    EXPECT_EQ(result.count, 3u);
    ByteString str = "Hello World";
    EXPECT_EQ(result.matches.at(0).view, str.view());
    EXPECT_EQ(result.matches.at(1).view, "Test");
    EXPECT_EQ(result.matches.at(2).view, "1234");
}

TEST_CASE(catch_all_newline_2)
{
    Regex<PosixExtended> re("^.*$");
    RegexResult result;
    result = match("Hello World\nTest\n1234\n"sv, re, PosixFlags::Multiline);
    EXPECT_EQ(result.success, true);
    EXPECT_EQ(result.count, 3u);
    EXPECT_EQ(result.matches.at(0).view, "Hello World");
    EXPECT_EQ(result.matches.at(1).view, "Test");
    EXPECT_EQ(result.matches.at(2).view, "1234");

    result = match("Hello World\nTest\n1234\n"sv, re);
    EXPECT_EQ(result.success, true);
    EXPECT_EQ(result.count, 1u);
    EXPECT_EQ(result.matches.at(0).view, "Hello World\nTest\n1234\n");
}

TEST_CASE(match_all_character_class)
{
    Regex<PosixExtended> re("[[:alpha:]]");
    ByteString str = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    RegexResult result = match(str, re, PosixFlags::Global);

    EXPECT_EQ(result.success, true);
    EXPECT_EQ(result.count, 24u);
    EXPECT_EQ(result.matches.at(0).view, "W");
    EXPECT_EQ(result.matches.at(1).view, "i");
    EXPECT_EQ(result.matches.at(2).view, "n");
}

TEST_CASE(match_character_class_with_assertion)
{
    Regex<PosixExtended> re("[[:alpha:]]+$");
    ByteString str = "abcdef";
    RegexResult result = match(str, re);

    EXPECT_EQ(result.success, true);
    EXPECT_EQ(result.count, 1u);
}

TEST_CASE(example_for_git_commit)
{
    Regex<PosixExtended> re("^.*$");
    auto result = re.match("Well, hello friends!\nHello World!"sv);

    EXPECT(result.success);
    EXPECT(result.count == 1);
    EXPECT(result.matches.at(0).view.starts_with("Well"sv));
    EXPECT(result.matches.at(0).view.length() == 33);

    EXPECT(re.has_match("Well,...."sv));

    result = re.match("Well, hello friends!\nHello World!"sv, PosixFlags::Multiline);

    EXPECT(result.success);
    EXPECT(result.count == 2);
    EXPECT(result.matches.at(0).view == "Well, hello friends!");
    EXPECT(result.matches.at(1).view == "Hello World!");
}

TEST_CASE(email_address)
{
    Regex<PosixExtended> re("^[A-Z0-9a-z._%+-]{1,64}@([A-Za-z0-9-]{1,63}\\.){1,125}[A-Za-z]{2,63}$");
    EXPECT(re.has_match("hello.world@domain.tld"sv));
    EXPECT(re.has_match("this.is.a.very_long_email_address@world.wide.web"sv));
}

TEST_CASE(ini_file_entries)
{
    Regex<PosixExtended> re("[[:alpha:]]*=([[:digit:]]*)|\\[(.*)\\]");
    RegexResult result;

    if constexpr (REGEX_DEBUG) {
        RegexDebug regex_dbg(stderr);
        regex_dbg.print_raw_bytecode(re);
        regex_dbg.print_header();
        regex_dbg.print_bytecode(re);
    }

    ByteString haystack = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    EXPECT_EQ(re.search(haystack.view(), result, PosixFlags::Multiline), true);
    EXPECT_EQ(result.count, 3u);

    if constexpr (REGEX_DEBUG) {
        for (auto& v : result.matches)
            fprintf(stderr, "%s\n", v.view.to_byte_string().characters());
    }

    EXPECT_EQ(result.matches.at(0).view, "[Window]");
    EXPECT_EQ(result.capture_group_matches.at(0).at(0).view, "Window");
    EXPECT_EQ(result.matches.at(1).view, "Opacity=255");
    EXPECT_EQ(result.matches.at(1).line, 1u);
    EXPECT_EQ(result.matches.at(1).column, 0u);
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).view, "255");
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).line, 1u);
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).column, 8u);
    EXPECT_EQ(result.matches.at(2).view, "AudibleBeep=0");
    EXPECT_EQ(result.capture_group_matches.at(2).at(0).view, "0");
    EXPECT_EQ(result.capture_group_matches.at(2).at(0).line, 2u);
    EXPECT_EQ(result.capture_group_matches.at(2).at(0).column, 12u);
}

TEST_CASE(ini_file_entries2)
{
    Regex<PosixExtended> re("[[:alpha:]]*=([[:digit:]]*)");
    RegexResult result;

    ByteString haystack = "ViewMode=Icon";

    EXPECT_EQ(re.match(haystack.view(), result), false);
    EXPECT_EQ(result.count, 0u);

    EXPECT_EQ(re.search(haystack.view(), result), true);
    EXPECT_EQ(result.count, 1u);
}

TEST_CASE(named_capture_group)
{
    Regex<PosixExtended> re("[[:alpha:]]*=(?<Test>[[:digit:]]*)");
    RegexResult result;

    if constexpr (REGEX_DEBUG) {
        RegexDebug regex_dbg(stderr);
        regex_dbg.print_raw_bytecode(re);
        regex_dbg.print_header();
        regex_dbg.print_bytecode(re);
    }

    ByteString haystack = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    EXPECT_EQ(re.search(haystack, result, PosixFlags::Multiline), true);
    EXPECT_EQ(result.count, 2u);
    EXPECT_EQ(result.matches.at(0).view, "Opacity=255");
    EXPECT_EQ(result.capture_group_matches.at(0).at(0).view, "255");
    EXPECT_EQ(result.capture_group_matches.at(0).at(0).capture_group_name, "Test");
    EXPECT_EQ(result.matches.at(1).view, "AudibleBeep=0");
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).view, "0");
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).capture_group_name, "Test");
}

TEST_CASE(ecma262_named_capture_group_with_dollar_sign)
{
    Regex<ECMA262> re("[a-zA-Z]*=(?<$Test$>[0-9]*)");
    RegexResult result;

    if constexpr (REGEX_DEBUG) {
        RegexDebug regex_dbg(stderr);
        regex_dbg.print_raw_bytecode(re);
        regex_dbg.print_header();
        regex_dbg.print_bytecode(re);
    }

    ByteString haystack = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    EXPECT_EQ(re.search(haystack, result, ECMAScriptFlags::Multiline), true);
    EXPECT_EQ(result.count, 2u);
    EXPECT_EQ(result.matches.at(0).view, "Opacity=255");
    EXPECT_EQ(result.capture_group_matches.at(0).at(0).view, "255");
    EXPECT_EQ(result.capture_group_matches.at(0).at(0).capture_group_name, "$Test$");
    EXPECT_EQ(result.matches.at(1).view, "AudibleBeep=0");
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).view, "0");
    EXPECT_EQ(result.capture_group_matches.at(1).at(0).capture_group_name, "$Test$");
}

TEST_CASE(a_star)
{
    Regex<PosixExtended> re("a*");
    RegexResult result;

    if constexpr (REGEX_DEBUG) {
        RegexDebug regex_dbg(stderr);
        regex_dbg.print_raw_bytecode(re);
        regex_dbg.print_header();
        regex_dbg.print_bytecode(re);
    }

    ByteString haystack = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    EXPECT_EQ(re.search(haystack.view(), result, PosixFlags::Multiline), true);
    EXPECT_EQ(result.count, 32u);
    if (result.count == 32u) {
        EXPECT_EQ(result.matches.at(0).view.length(), 0u);
        EXPECT_EQ(result.matches.at(10).view.length(), 1u);
        EXPECT_EQ(result.matches.at(10).view, "a");
        EXPECT_EQ(result.matches.at(31).view.length(), 0u);
    }
}

TEST_CASE(simple_period_end_benchmark)
{
    Regex<PosixExtended> re("hello.$");
    RegexResult m;
    EXPECT_EQ(re.search("Hello1"sv, m), false);
    EXPECT_EQ(re.search("hello1hello1"sv, m), true);
    EXPECT_EQ(re.search("hello2hell"sv, m), false);
    EXPECT_EQ(re.search("hello?"sv, m), true);
}

TEST_CASE(posix_extended_nested_capture_group)
{
    Regex<PosixExtended> re("(h(e(?<llo>llo)))"); // group 0 -> "hello", group 1 -> "ello", group 2/"llo" -> "llo"
    auto result = re.match("hello"sv);
    EXPECT(result.success);
    EXPECT_EQ(result.capture_group_matches.size(), 1u);
    EXPECT_EQ(result.capture_group_matches[0].size(), 3u);
    EXPECT_EQ(result.capture_group_matches[0][0].view, "hello"sv);
    EXPECT_EQ(result.capture_group_matches[0][1].view, "ello"sv);
    EXPECT_EQ(result.capture_group_matches[0][2].view, "llo"sv);
}

auto parse_test_case_long_disjunction_chain = ByteString::repeated("a|"sv, 100000);

TEST_CASE(ECMA262_parse)
{
    struct _test {
        StringView pattern;
        regex::Error expected_error { regex::Error::NoError };
        regex::ECMAScriptFlags flags {};
    };

    _test const tests[] {
        { "^hello.$"sv },
        { "^(hello.)$"sv },
        { "^h{0,1}ello.$"sv },
        { "^hello\\W$"sv },
        { "^hell\\w.$"sv },
        { "^hell\\x6f1$"sv }, // ^hello1$
        { "^hel(?:l\\w).$"sv },
        { "^hel(?<LO>l\\w).$"sv },
        { "^[-a-zA-Z\\w\\s]+$"sv },
        { "\\bhello\\B"sv },
        { "^[\\w+/_-]+[=]{0,2}$"sv },                        // #4189
        { "^(?:[^<]*(<[\\w\\W]+>)[^>]*$|#([\\w\\-]*)$)"sv }, // #4189
        { "\\/"sv },                                         // #4189
        { ",/=-:"sv },                                       // #4243
        { "\\x"sv },                                         // Even invalid escapes are allowed if ~unicode.
        { "\\x1"sv },                                        // Even invalid escapes are allowed if ~unicode.
        { "\\x1"sv, regex::Error::InvalidPattern, regex::ECMAScriptFlags::Unicode },
        { "\\x11"sv },
        { "\\x11"sv, regex::Error::NoError, regex::ECMAScriptFlags::Unicode },
        { "\\"sv, regex::Error::InvalidTrailingEscape },
        { "(?"sv, regex::Error::InvalidCaptureGroup },
        { "\\u1234"sv, regex::Error::NoError, regex::ECMAScriptFlags::Unicode },
        { "[\\u1234]"sv, regex::Error::NoError, regex::ECMAScriptFlags::Unicode },
        { "\\u1"sv, regex::Error::InvalidPattern, regex::ECMAScriptFlags::Unicode },
        { "[\\u1]"sv, regex::Error::InvalidPattern, regex::ECMAScriptFlags::Unicode },
        { ",(?"sv, regex::Error::InvalidCaptureGroup }, // #4583
        { "{1}"sv, regex::Error::InvalidPattern },
        { "{1,2}"sv, regex::Error::InvalidPattern },
        { "\\uxxxx"sv, regex::Error::NoError },
        { "\\uxxxx"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\ud83d"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "\\ud83d\\uxxxx"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\u{0}"sv },
        { "\\u{0}"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "\\u{10ffff}"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "\\u{10ffff"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\u{10ffffx"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\u{110000}"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\p"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\p{"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\p{}"sv, regex::Error::InvalidNameForProperty, ECMAScriptFlags::Unicode },
        { "\\p{AsCiI}"sv, regex::Error::InvalidNameForProperty, ECMAScriptFlags::Unicode },
        { "\\p{hello friends}"sv, regex::Error::InvalidNameForProperty, ECMAScriptFlags::Unicode },
        { "\\p{Prepended_Concatenation_Mark}"sv, regex::Error::InvalidNameForProperty, ECMAScriptFlags::Unicode },
        { "\\p{ASCII}"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "\\\\p{1}"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "\\\\p{AsCiI}"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\\\p{ASCII}"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\c"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "\\c"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "[\\c]"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "[\\c]"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\c`"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "\\c`"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "[\\c`]"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "[\\c`]"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\A"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "\\A"sv, regex::Error::InvalidCharacterClass, ECMAScriptFlags::Unicode },
        { "[\\A]"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "[\\A]"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\0"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "\\0"sv, regex::Error::NoError, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::BrowserExtended) },
        { "\\00"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "\\00"sv, regex::Error::InvalidCharacterClass, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::BrowserExtended) },
        { "[\\0]"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "[\\0]"sv, regex::Error::NoError, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::BrowserExtended) },
        { "[\\00]"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "[\\00]"sv, regex::Error::InvalidPattern, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::BrowserExtended) },
        { "\\^\\$\\\\\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\/"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "[\\^\\$\\\\\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\/]"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "]"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "]"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\]"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "}"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },
        { "}"sv, regex::Error::InvalidPattern, ECMAScriptFlags::Unicode },
        { "\\}"sv, regex::Error::NoError, ECMAScriptFlags::Unicode },
        { "a{9007199254740991}"sv }, // 2^53 - 1
        { "a{9007199254740991,}"sv },
        { "a{9007199254740991,9007199254740991}"sv },
        { "a{9007199254740992}"sv, regex::Error::InvalidBraceContent },
        { "a{9007199254740992,}"sv, regex::Error::InvalidBraceContent },
        { "a{9007199254740991,9007199254740992}"sv, regex::Error::InvalidBraceContent },
        { "a{9007199254740992,9007199254740991}"sv, regex::Error::InvalidBraceContent },
        { "a{9007199254740992,9007199254740992}"sv, regex::Error::InvalidBraceContent },
        { "(?<a>a)(?<a>b)"sv, regex::Error::DuplicateNamedCapture },
        { "(?<a>a)(?<b>b)(?<a>c)"sv, regex::Error::DuplicateNamedCapture },
        { "(?<a>(?<a>a))"sv, regex::Error::DuplicateNamedCapture },
        { "(?:(?<x>a)|(?<y>a)(?<x>b))(?:(?<z>c)|(?<z>d))"sv }, // Duplicate named capturing groups in separate alternatives should parse correctly
        { "(?<1a>a)"sv, regex::Error::InvalidNameForCaptureGroup },
        { "(?<\\a>a)"sv, regex::Error::InvalidNameForCaptureGroup },
        { "(?<\ta>a)"sv, regex::Error::InvalidNameForCaptureGroup },
        { "(?<$$_$$>a)"sv },
        { "(?<ÿ>a)"sv },
        { "(?<𝓑𝓻𝓸𝔀𝓷>a)"sv },
        { "((?=lg)?[vl]k\\-?\\d{3}) bui| 3\\.[-\\w; ]{10}lg?-([06cv9]{3,4})"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended }, // #12373, quantifiable assertions.
        { parse_test_case_long_disjunction_chain.view() },                                                                                 // A whole lot of disjunctions, should not overflow the stack.
        { "(\"|')(?:(?!\\2)[^\\\\\\r\\n]|\\\\.)*\\2"sv, regex::Error::NoError, ECMAScriptFlags::BrowserExtended },                         // LegacyOctalEscapeSequence should not consume too many chars (and should not crash)
        // #18324, Capture group counter skipped past EOF.
        { "\\1[\\"sv, regex::Error::InvalidNumber },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.pattern, test.flags);
        EXPECT_EQ(re.parser_result.error, test.expected_error);
        if constexpr (REGEX_DEBUG) {
            dbgln("\n");
            RegexDebug regex_dbg(stderr);
            regex_dbg.print_raw_bytecode(re);
            regex_dbg.print_header();
            regex_dbg.print_bytecode(re);
            dbgln("\n");
        }
    }
}

TEST_CASE(ECMA262_match)
{
    constexpr auto global_multiline = ECMAScriptFlags::Global | ECMAScriptFlags::Multiline;

    struct _test {
        StringView pattern;
        StringView subject;
        bool matches { true };
        ECMAScriptFlags options {};
    };
    constexpr _test tests[] {
        { "^hello.$"sv, "hello1"sv },
        { "^(hello.)$"sv, "hello1"sv },
        { "^h{0,1}ello.$"sv, "ello1"sv },
        { "^hello\\W$"sv, "hello!"sv },
        { "^hell\\w.$"sv, "hellx!"sv },
        { "^hell\\x6f1$"sv, "hello1"sv },
        { "^hel(?<LO>l.)1$"sv, "hello1"sv },
        { "^hel(?<LO>l.)1*\\k<LO>.$"sv, "hello1lo1"sv },
        { "^[-a-z1-3\\s]+$"sv, "hell2 o1"sv },
        { "^[\\0-\\x1f]$"sv, "\n"sv },
        { .pattern = "\\bhello\\B"sv, .subject = "hello1"sv, .options = ECMAScriptFlags::Global },
        { "\\b.*\\b"sv, "hello1"sv },
        { "[^\\D\\S]{2}"sv, "1 "sv, false },
        { "bar(?=f.)foo"sv, "barfoo"sv },
        { "bar(?=foo)bar"sv, "barbar"sv, false },
        { "bar(?!foo)bar"sv, "barbar"sv, true },
        { "bar(?!bar)bar"sv, "barbar"sv, false },
        { "bar.*(?<=foo)"sv, "barbar"sv, false },
        { "bar.*(?<!foo)"sv, "barbar"sv, true },
        { "((...)X)+"sv, "fooXbarXbazX"sv, true },
        { "(?:)"sv, ""sv, true },
        { "\\^"sv, "^"sv },
        { "\\^\\$\\\\\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\/"sv, "^$\\.*+?()[]{}|/"sv, true, ECMAScriptFlags::Unicode },
        { "[\\^\\$\\\\\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\/]{15}"sv, "^$\\.*+?()[]{}|/"sv, true, ECMAScriptFlags::Unicode },
        { "(a{2}){3}"sv, "aaaaaa"sv },
        { "(a{2}){3}"sv, "aaaabaa"sv, false },
        { "(a{2}){4}"sv, "aaaaaaaa"sv },
        { "(a{2}){4}"sv, "aaaaaabaa"sv, false },
        { "(a{3}){2}"sv, "aaaaaa"sv },
        { "(a{3}){2}"sv, "aaaabaa"sv, false },
        { "(a{4}){2}"sv, "aaaaaaaa"sv },
        { "(a{4}){2}"sv, "aaaaaabaa"sv, false },
        { "\\u{4}"sv, "uuuu"sv },
        { "(?<=.{3})f"sv, "abcdef"sv, true, (ECMAScriptFlags)regex::AllFlags::Global },
        { "(?<=.{3})f"sv, "abc😀ef"sv, true, (ECMAScriptFlags)regex::AllFlags::Global },
        // ECMA262, B.1.4. Regular Expression Pattern extensions for browsers
        { "{"sv, "{"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\5"sv, "\5"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\05"sv, "\5"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\455"sv, "\0455"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\314"sv, "\314"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\c"sv, "\\c"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\cf"sv, "\06"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\c1"sv, "\\c1"sv, true, ECMAScriptFlags::BrowserExtended },
        { "[\\c1]"sv, "\x11"sv, true, ECMAScriptFlags::BrowserExtended },
        { "[\\w-\\d]"sv, "-"sv, true, ECMAScriptFlags::BrowserExtended },
        // #5517, appears to be matching JS expressions that involve regular expressions...
        {
            "^(?:^^\\.?|[!+-]|!=|!==|#|%|%=|&|&&|&&=|&=|\\(|\\*|\\*=|\\+=|,|-=|->|\\/|\\/=|:|::|;|<|<<|<<=|<=|=|==|===|>|>=|>>|>>=|>>>|>>>=|[?@[^]|\\^=|\\^\\^|\\^\\^=|{|\\||\\|=|\\|\\||\\|\\|=|~|break|case|continue|delete|do|else|finally|instanceof|return|throw|try|typeof)\\s*(\\/(?=[^*/])(?:[^/[\\\\]|\\\\[\\S\\s]|\\[(?:[^\\\\\\]]|\\\\[\\S\\s])*(?:]|$))+\\/)"sv,
            "return /xx/"sv,
            true,
            ECMAScriptFlags::BrowserExtended,
        },
        // #5518
        { "a{2,}"sv, "aaaa"sv },
        { "\\0"sv, "\0"sv, true, ECMAScriptFlags::BrowserExtended },
        { "\\0"sv, "\0"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::BrowserExtended) },
        { "\\01"sv, "\1"sv, true, ECMAScriptFlags::BrowserExtended },
        { "[\\0]"sv, "\0"sv, true, ECMAScriptFlags::BrowserExtended },
        { "[\\0]"sv, "\0"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::BrowserExtended) },
        { "[\\01]"sv, "\1"sv, true, ECMAScriptFlags::BrowserExtended },
        // #9686, Should allow null bytes in pattern
        { "(\0|a)"sv, "a"sv, true },
        // #6042, Groups inside lookarounds may be referenced outside, but their contents appear empty if the pattern in the lookaround fails.
        { "(.*?)a(?!(a+)b\\2c)\\2(.*)"sv, "baaabaac"sv, true },
        // #11940, Global (not the 'g' flag) regexps should attempt to match the zero-length end of the string too.
        { "a|$"sv, "x"sv, true, (ECMAScriptFlags)regex::AllFlags::Global },
        // #12126, ECMA262 regexp should match literal newlines without the 's' flag.
        { "foo\nbar"sv, "foo\nbar"sv, true },
        // #12126, ECMA262 regexp should match newline with [^].
        { "foo[^]bar"sv, "foo\nbar"sv, true },
        // Insensitive lookup table: characters in a range do not necessarily lie in the same range after being converted to lowercase.
        { "^[_A-Z]+$"sv, "_aA"sv, true, ECMAScriptFlags::Insensitive },
        { "^[a-sy-z]$"sv, "b"sv, true, ECMAScriptFlags::Insensitive },
        { "^[a-sy-z]$"sv, "y"sv, true, ECMAScriptFlags::Insensitive },
        { "^[a-sy-z]$"sv, "u"sv, false, ECMAScriptFlags::Insensitive },
        // Dot should not match any of CR/LF/LS/PS in ECMA262 mode without DotAll.
        { "."sv, "\n\r\u2028\u2029"sv, false },
        // $ should accept all LineTerminators in ECMA262 mode with Multiline.
        { "a$"sv, "a\r\n"sv, true, global_multiline.value() },
        { "^a"sv, "\ra"sv, true, global_multiline.value() },
        { "^(.*?):[ \\t]*([^\\r\\n]*)$"sv, "content-length: 488\r\ncontent-type: application/json; charset=utf-8\r\n"sv, true, global_multiline.value() },
        // ladybird#968, ?+ should not loop forever. */
        { "^\\?((&?category=[0-9]+)?(&?shippable=1)?(&?ad_type=demand)?(&?page=[0-9]+)?(&?locations=(r|d)_[0-9]+)?)+$"sv, "?category=54&shippable=1&baby_age=p,0,1,3"sv, false },
        // optimizer bug, blindly accepting inverted char classes [^x] as atomic rewrite opportunities.
        { "([^\\s]+):\\s*([^;]+);"sv, "font-family: 'Inter';"sv, true },
        // Optimizer bug, ignoring references that weren't bound in the current or past block, ladybird#2281
        { "(a)(?=a*\\1)"sv, "aaaa"sv, true, global_multiline.value() },
        // Optimizer bug, wrong Repeat basic block splits.
        { "[ a](b{2})"sv, "abb"sv, true },
        // See above.
        { "^ {0,3}(([\\`\\~])\\2{2,})\\s*([\\*_]*)\\s*([^\\*_\\s]*).*$"sv, ""sv, false },
        // See above, also ladybird#2931.
        {
            "^(\\d{4}|[+-]\\d{6})(?:-?(\\d{2})(?:-?(\\d{2}))?)?(?:[ T]?(\\d{2}):?(\\d{2})(?::?(\\d{2})(?:[,.](\\d{1,}))?)?(?:(Z)|([+-])(\\d{2})(?::?(\\d{2}))?)?)?$"sv,
            ""sv,
            false,
        },
        // Optimizer bug, ignoring an enabled trailing 'invert' when comparing blocks, ladybird#3421.
        { "[^]*[^]"sv, "i"sv, true },
        { "xx|...|...."sv, "cd"sv, false },
        // Tests nested lookahead with alternation - verifies proper save/restore stack cleanup
        { "a(?=.(?=c)|b)b"sv, "ab"sv, true },
        { "(?=)(?=\\d)"sv, "smart"sv, false },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.pattern, test.options);
        if constexpr (REGEX_DEBUG) {
            dbgln("\n");
            RegexDebug regex_dbg(stderr);
            regex_dbg.print_raw_bytecode(re);
            regex_dbg.print_header();
            regex_dbg.print_bytecode(re);
            dbgln("\n");
        }
        EXPECT_EQ(re.parser_result.error, regex::Error::NoError);
        EXPECT_EQ(re.match(test.subject).success, test.matches);
    }
}

TEST_CASE(ECMA262_unicode_match)
{
    constexpr auto space_and_line_terminator_code_points = Array { 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x0020, 0x00A0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF };

    StringBuilder builder;
    for (u32 code_point : space_and_line_terminator_code_points)
        builder.append_code_point(code_point);
    auto space_and_line_terminators = builder.to_byte_string();

    struct _test {
        StringView pattern;
        StringView subject;
        bool matches { true };
        ECMAScriptFlags options {};
    };
    _test tests[] {
        { "\xf0\x9d\x8c\x86"sv, "abcdef"sv, false, ECMAScriptFlags::Unicode },
        { "[\xf0\x9d\x8c\x86]"sv, "abcdef"sv, false, ECMAScriptFlags::Unicode },
        { "\\ud83d"sv, "😀"sv, true },
        { "\\ud83d"sv, "😀"sv, false, ECMAScriptFlags::Unicode },
        { "\\ude00"sv, "😀"sv, true },
        { "\\ude00"sv, "😀"sv, false, ECMAScriptFlags::Unicode },
        { "\\ud83d\\ude00"sv, "😀"sv, true },
        { "\\ud83d\\ude00"sv, "😀"sv, true, ECMAScriptFlags::Unicode },
        { "\\u{1f600}"sv, "😀"sv, true, ECMAScriptFlags::Unicode },
        { "\\ud83d\\ud83d"sv, "\xed\xa0\xbd\xed\xa0\xbd"sv, true },
        { "\\ud83d\\ud83d"sv, "\xed\xa0\xbd\xed\xa0\xbd"sv, true, ECMAScriptFlags::Unicode },
        { "(?<=.{3})f"sv, "abcdef"sv, true, ECMAScriptFlags::Unicode },
        { "(?<=.{3})f"sv, "abc😀ef"sv, true, ECMAScriptFlags::Unicode },
        { "(?<𝓑𝓻𝓸𝔀𝓷>brown)"sv, "brown"sv, true, ECMAScriptFlags::Unicode },
        { "(?<\\u{1d4d1}\\u{1d4fb}\\u{1d4f8}\\u{1d500}\\u{1d4f7}>brown)"sv, "brown"sv, true, ECMAScriptFlags::Unicode },
        { "(?<\\ud835\\udcd1\\ud835\\udcfb\\ud835\\udcf8\\ud835\\udd00\\ud835\\udcf7>brown)"sv, "brown"sv, true, ECMAScriptFlags::Unicode },
        { "^\\s+$"sv, space_and_line_terminators },
        { "^\\s+$"sv, space_and_line_terminators, true, ECMAScriptFlags::Unicode },
        { "[\\u0390]"sv, "\u1fd3"sv, false, ECMAScriptFlags::Unicode },
        { "[\\u1fd3]"sv, "\u0390"sv, false, ECMAScriptFlags::Unicode },
        { "[\\u0390]"sv, "\u1fd3"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::Insensitive) },
        { "[\\u1fd3]"sv, "\u0390"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::Insensitive) },
        { "[\\u03b0]"sv, "\u1fe3"sv, false, ECMAScriptFlags::Unicode },
        { "[\\u1fe3]"sv, "\u03b0"sv, false, ECMAScriptFlags::Unicode },
        { "[\\u03b0]"sv, "\u1fe3"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::Insensitive) },
        { "[\\u1fe3]"sv, "\u03b0"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::Insensitive) },
        { "[\\ufb05]"sv, "\ufb06"sv, false, ECMAScriptFlags::Unicode },
        { "[\\ufb06]"sv, "\ufb05"sv, false, ECMAScriptFlags::Unicode },
        { "[\\ufb05]"sv, "\ufb06"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::Insensitive) },
        { "[\\ufb06]"sv, "\ufb05"sv, true, combine_flags(ECMAScriptFlags::Unicode, ECMAScriptFlags::Insensitive) },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.pattern, (ECMAScriptFlags)regex::AllFlags::Global | test.options);

        auto subject = MUST(AK::utf8_to_utf16(test.subject));
        Utf16View view { subject };

        if constexpr (REGEX_DEBUG) {
            dbgln("\n");
            RegexDebug regex_dbg(stderr);
            regex_dbg.print_raw_bytecode(re);
            regex_dbg.print_header();
            regex_dbg.print_bytecode(re);
            dbgln("\n");
        }

        EXPECT_EQ(re.parser_result.error, regex::Error::NoError);
        EXPECT_EQ(re.match(view).success, test.matches);
    }
}

TEST_CASE(ECMA262_unicode_sets_parser_error)
{
    struct _test {
        StringView pattern;
        regex::Error error;
    };

    constexpr _test tests[] {
        { "[[]"sv, regex::Error::InvalidPattern },
        { "[[x[]]]"sv, regex::Error::NoError }, // #23691, should not crash on empty charclass within AndOr.
    };

    for (auto test : tests) {
        Regex<ECMA262> re(test.pattern, (ECMAScriptFlags)regex::AllFlags::UnicodeSets);
        EXPECT_EQ(re.parser_result.error, test.error);
    }
}

TEST_CASE(ECMA262_unicode_sets_match)
{
    struct _test {
        StringView pattern;
        StringView subject;
        bool matches { true };
        ECMAScriptFlags options {};
    };

    constexpr _test tests[] {
        { "[\\w--x]"sv, "x"sv, false },
        { "[\\w&&x]"sv, "y"sv, false },
        { "[\\w--x]"sv, "y"sv, true },
        { "[\\w&&x]"sv, "x"sv, true },
        { "[[0-9\\w]--x--6]"sv, "6"sv, false },
        { "[[0-9\\w]--x--6]"sv, "x"sv, false },
        { "[[0-9\\w]--x--6]"sv, "y"sv, true },
        { "[[0-9\\w]--x--6]"sv, "9"sv, true },
        { "[\\w&&\\d]"sv, "a"sv, false },
        { "[\\w&&\\d]"sv, "4"sv, true },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.pattern, (ECMAScriptFlags)regex::AllFlags::UnicodeSets | test.options);
        if constexpr (REGEX_DEBUG) {
            dbgln("\n");
            RegexDebug regex_dbg(stderr);
            regex_dbg.print_raw_bytecode(re);
            regex_dbg.print_header();
            regex_dbg.print_bytecode(re);
            dbgln("\n");
        }

        EXPECT_EQ(re.parser_result.error, regex::Error::NoError);
        auto result = re.match(test.subject).success;
        EXPECT_EQ(result, test.matches);
    }
}

TEST_CASE(ECMA262_property_match)
{
    struct _test {
        StringView pattern;
        StringView subject;
        bool matches { true };
        ECMAScriptFlags options {};
    };

    constexpr _test tests[] {
        { "\\p{ASCII}"sv, "a"sv, false },
        { "\\p{ASCII}"sv, "p{ASCII}"sv, true },
        { "\\p{ASCII}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{ASCII}"sv, "😀"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{ASCII}"sv, "a"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{ASCII}"sv, "😀"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{ASCII_Hex_Digit}"sv, "1"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{ASCII_Hex_Digit}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{ASCII_Hex_Digit}"sv, "x"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{ASCII_Hex_Digit}"sv, "1"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{ASCII_Hex_Digit}"sv, "a"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{ASCII_Hex_Digit}"sv, "x"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{Any}"sv, "\xcd\xb8"sv, true, ECMAScriptFlags::Unicode },       // U+0378, which is an unassigned code point.
        { "\\P{Any}"sv, "\xcd\xb8"sv, false, ECMAScriptFlags::Unicode },      // U+0378, which is an unassigned code point.
        { "\\p{Assigned}"sv, "\xcd\xb8"sv, false, ECMAScriptFlags::Unicode }, // U+0378, which is an unassigned code point.
        { "\\P{Assigned}"sv, "\xcd\xb8"sv, true, ECMAScriptFlags::Unicode },  // U+0378, which is an unassigned code point.
        { "\\p{Lu}"sv, "a"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{Lu}"sv, "A"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{Lu}"sv, "9"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{Cased_Letter}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{Cased_Letter}"sv, "A"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{Cased_Letter}"sv, "9"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{Cased_Letter}"sv, "a"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{Cased_Letter}"sv, "A"sv, false, ECMAScriptFlags::Unicode },
        { "\\P{Cased_Letter}"sv, "9"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{General_Category=Cased_Letter}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{General_Category=Cased_Letter}"sv, "A"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{General_Category=Cased_Letter}"sv, "9"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{gc=Cased_Letter}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{gc=Cased_Letter}"sv, "A"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{gc=Cased_Letter}"sv, "9"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{Script=Latin}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{Script=Latin}"sv, "A"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{Script=Latin}"sv, "9"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{sc=Latin}"sv, "a"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{sc=Latin}"sv, "A"sv, true, ECMAScriptFlags::Unicode },
        { "\\p{sc=Latin}"sv, "9"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{Script_Extensions=Deva}"sv, "a"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{Script_Extensions=Beng}"sv, "\xe1\xb3\x95"sv, true, ECMAScriptFlags::Unicode }, // U+01CD5
        { "\\p{Script_Extensions=Deva}"sv, "\xe1\xb3\x95"sv, true, ECMAScriptFlags::Unicode }, // U+01CD5
        { "\\p{scx=Deva}"sv, "a"sv, false, ECMAScriptFlags::Unicode },
        { "\\p{scx=Beng}"sv, "\xe1\xb3\x95"sv, true, ECMAScriptFlags::Unicode }, // U+01CD5
        { "\\p{scx=Deva}"sv, "\xe1\xb3\x95"sv, true, ECMAScriptFlags::Unicode }, // U+01CD5
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.pattern, (ECMAScriptFlags)regex::AllFlags::Global | regex::ECMAScriptFlags::BrowserExtended | test.options);

        auto subject = MUST(AK::utf8_to_utf16(test.subject));
        Utf16View view { subject };

        if constexpr (REGEX_DEBUG) {
            dbgln("\n");
            RegexDebug regex_dbg(stderr);
            regex_dbg.print_raw_bytecode(re);
            regex_dbg.print_header();
            regex_dbg.print_bytecode(re);
            dbgln("\n");
        }

        EXPECT_EQ(re.parser_result.error, regex::Error::NoError);
        EXPECT_EQ(re.match(view).success, test.matches);
    }
}

TEST_CASE(replace)
{
    struct _test {
        StringView pattern;
        StringView replacement;
        StringView subject;
        StringView expected;
        ECMAScriptFlags options {};
    };

    constexpr _test tests[] {
        { "foo(.+)"sv, "aaa"sv, "test"sv, "test"sv },
        { "foo(.+)"sv, "test\\1"sv, "foobar"sv, "testbar"sv },
        { "foo(.+)"sv, "\\2\\1"sv, "foobar"sv, "\\2bar"sv },
        { "foo(.+)"sv, "\\\\\\1"sv, "foobar"sv, "\\bar"sv },
        { "foo(.)"sv, "a\\1"sv, "fooxfooy"sv, "axay"sv, ECMAScriptFlags::Multiline },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.pattern, test.options);
        if constexpr (REGEX_DEBUG) {
            dbgln("\n");
            RegexDebug regex_dbg(stderr);
            regex_dbg.print_raw_bytecode(re);
            regex_dbg.print_header();
            regex_dbg.print_bytecode(re);
            dbgln("\n");
        }
        EXPECT_EQ(re.parser_result.error, regex::Error::NoError);
        EXPECT_EQ(re.replace(test.subject, test.replacement), test.expected);
    }
}

TEST_CASE(case_insensitive_match)
{
    Regex<PosixExtended> re("cd", PosixFlags::Insensitive | PosixFlags::Global);
    auto result = re.match("AEKFCD"sv);

    EXPECT_EQ(result.success, true);
    if (result.success) {
        EXPECT_EQ(result.matches.at(0).column, 4ul);
    }
}

TEST_CASE(extremely_long_fork_chain)
{
    Regex<ECMA262> re("(?:aa)*");
    auto input = ByteString::repeated('a', 1000);
    auto result = re.match(input);
    EXPECT_EQ(result.success, true);
}

TEST_CASE(nullable_quantifiers)
{
    Regex<ECMA262> re("(a?b?\x3f)*"); // Pattern (a?b??)* isn't written plain to avoid "??)", which is a trigraph.
    auto result = re.match("ab"sv);
    EXPECT_EQ(result.matches.at(0).view, "ab"sv);
}

TEST_CASE(theoretically_infinite_loop)
{
    Array patterns {
        "(a*)*"sv,  // Infinitely matching empty substrings, the outer loop should short-circuit.
        "(a*?)*"sv, // Infinitely matching empty substrings, the outer loop should short-circuit.
        "(a*)*?"sv, // Should match exactly nothing.
        "(?:)*?"sv, // Should not generate an infinite fork loop.
        "(a?)+$"sv, // Infinitely matching empty strings, but with '+' instead of '*'.
    };
    for (auto& pattern : patterns) {
        Regex<ECMA262> re(pattern);
        auto result = re.match(""sv);
        EXPECT_EQ(result.success, true);
    }
}

static auto g_lots_of_a_s = ByteString::repeated('a', 10'000'000);

BENCHMARK_CASE(fork_performance)
{
    {
        Regex<ECMA262> re("(?:aa)*");
        auto result = re.match(g_lots_of_a_s);
        EXPECT_EQ(result.success, true);
    }
    {
        Regex<ECMA262> re("(a+)+b");
        auto result = re.match(g_lots_of_a_s.substring_view(0, 100));
        EXPECT_EQ(result.success, false);
    }
    {
        Regex<ECMA262> re("^(a|a?)+$");
        auto input = ByteString::formatted("{}b", g_lots_of_a_s.substring_view(0, 100));
        auto result = re.match(input);
        EXPECT_EQ(result.success, false);
    }
}

BENCHMARK_CASE(anchor_performance)
{
    Regex<ECMA262> re("^b");
    for (auto i = 0; i < 100'000; i++) {
        auto result = re.match(g_lots_of_a_s);
        EXPECT_EQ(result.success, false);
    }
}

TEST_CASE(optimizer_atomic_groups)
{
    Array tests {
        // Fork -> ForkReplace
        Tuple { "a*b"sv, "aaaaa"sv, false },
        Tuple { "a+b"sv, "aaaaa"sv, false },
        Tuple { "\\\\(\\d+)"sv, "\\\\"sv, false }, // Rewrite bug turning a+ to a*, see #10952.
        Tuple { "[a-z.]+\\."sv, "..."sv, true },   // Rewrite bug, incorrect interpretation of Compare.
        Tuple { "[.-]+\\."sv, ".-."sv, true },
        // Alternative fuse
        Tuple { "(abcfoo|abcbar|abcbaz).*x"sv, "abcbarx"sv, true },
        Tuple { "(a|a)"sv, "a"sv, true },
        Tuple { "(a|)"sv, ""sv, true },                   // Ensure that empty alternatives are not outright removed
        Tuple { "a{2,3}|a{5,8}"sv, "abc"sv, false },      // Optimizer should not mess up the instruction stream by ignoring inter-insn dependencies, see #11247.
        Tuple { "^(a{2,3}|a{5,8})$"sv, "aaaa"sv, false }, // Optimizer should not mess up the instruction stream by ignoring inter-insn dependencies, see #11247.
        // Optimizer should not chop off *half* of an instruction when fusing instructions.
        Tuple { "cubic-bezier\\(\\s*(-?\\d+\\.?\\d*|-?\\.\\d+)\\s*,\\s*(-?\\d+\\.?\\d*|-?\\.\\d+)\\s*,\\s*(-?\\d+\\.?\\d*|-?\\.\\d+)\\s*,\\s*(-?\\d+\\.?\\d*|-?\\.\\d+)\\s*\\)"sv, "cubic-bezier(.05, 0, 0, 1)"sv, true },
        // ForkReplace shouldn't be applied where it would change the semantics
        Tuple { "(1+)\\1"sv, "11"sv, true },
        Tuple { "(1+)1"sv, "11"sv, true },
        Tuple { "(1+)0"sv, "10"sv, true },
        // Rewrite should not skip over first required iteration of <x>+.
        Tuple { "a+"sv, ""sv, false },
        // 'y' and [^x] have an overlap ('y'), the loop should not be rewritten here.
        Tuple { "[^x]+y"sv, "ay"sv, true },
        // .+ should not be rewritten here, as it's followed by something that would be matched by `.`.
        Tuple { ".+(a|b|c)"sv, "xxa"sv, true },
        // (b+)(b+) produces an intermediate block with no matching ops, the optimiser should ignore that block when looking for following matches and correctly detect the overlap between (b+) and (b+).
        // note that the second loop may be rewritten to a ForkReplace, but the first loop should not be rewritten.
        Tuple { "(b+)(b+)"sv, "bbb"sv, true },
        // Don't treat [\S] as [\s]; see ladybird#2296.
        Tuple { "([^\\s]+?)\\(([\\s\\S]*)\\)"sv, "a(b)"sv, true },
        // Follow direct jumps in the optimizer instead of assuming they're a noop.
        Tuple { "(|[^]*)\\)"sv, "p)"sv, true },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.get<0>());
        auto result = re.match(test.get<1>());
        EXPECT_EQ(result.success, test.get<2>());
    }
}

TEST_CASE(optimizer_char_class_lut)
{
    Regex<ECMA262> re(R"([\f\n\r\t\v\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]+$)");

    if constexpr (REGEX_DEBUG) {
        dbgln("\n");
        RegexDebug regex_dbg(stderr);
        regex_dbg.print_raw_bytecode(re);
        regex_dbg.print_header();
        regex_dbg.print_bytecode(re);
        dbgln("\n");
    }

    // This will go through _all_ alternatives in the character class, and then fail.
    for (size_t i = 0; i < 1'000'000; ++i)
        EXPECT_EQ(re.match("1635488940000"sv).success, false);
}

TEST_CASE(optimizer_alternation)
{
    Array tests {
        // Pattern, Subject, Expected length [0 == fail]
        Tuple { "a|"sv, "a"sv, 1u },
        Tuple { "a|a|a|a|a|a|a|a|a|b"sv, "a"sv, 1u },
        Tuple { "ab|ac|ad|bc"sv, "bc"sv, 2u },
        // Should not crash on backwards jumps introduced by '.*'.
        Tuple { "\\bDroid\\b.*Build|XT912|XT928|XT926|XT915|XT919|XT925|XT1021|\\bMoto E\\b|XT1068|XT1092|XT1052"sv, "XT1068"sv, 6u },
        // Backwards jumps to IP 0 are normal jumps too.
        Tuple { "^(\\d+|x)"sv, "42"sv, 2u },
        // `Repeat' does not add its insn size to the jump target.
        Tuple { "[0-9]{2}|[0-9]"sv, "92"sv, 2u },
        // Don't ForkJump to the next instruction, rerunning it would produce the same result. see ladybird#2398.
        Tuple { "(xxxxxxxxxxxxxxxxxxxxxxx|xxxxxxxxxxxxxxxxxxxxxxx)?b"sv, "xxxxxxxxxxxxxxxxxxxxxxx"sv, 0u },
        // Don't take the jump in JumpNonEmpty with nonexistent checkpoints (also don't crash).
        Tuple { "(?!\\d*|[g-ta-r]+|[h-l]|\\S|\\S|\\S){,9}|\\S{7,8}|\\d|(?<wnvdfimiwd>)|[c-mj-tb-o]*|\\s"sv, "rjvogg7pm|li4nmct mjb2|pk7s8e0"sv, 0u },
    };

    for (auto& test : tests) {
        Regex<ECMA262> re(test.get<0>());
        auto result = re.match(test.get<1>());
        if (test.get<2>() != 0) {
            EXPECT(result.success);
            EXPECT_EQ(result.matches.first().view.length(), test.get<2>());
        } else {
            EXPECT(!result.success);
        }
    }
}

TEST_CASE(start_anchor)
{
    // Ensure that a circumflex at the start only matches the start of the line.
    {
        Regex<PosixBasic> re("^abc");
        EXPECT_EQ(re.match("123abcdef"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("abc123"sv, PosixFlags::Global).success, true);
        EXPECT_EQ(re.match("123^abcdef"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("^abc123"sv, PosixFlags::Global).success, false);

        // Multiple lines
        EXPECT_EQ(re.match("123\nabc"sv, PosixFlags::Multiline).success, true);
    }
}

TEST_CASE(posix_basic_dollar_is_end_anchor)
{
    // Ensure that a dollar sign at the end only matches the end of the line.
    {
        Regex<PosixBasic> re("abc$");
        EXPECT_EQ(re.match("123abcdef"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("123abc"sv, PosixFlags::Global).success, true);
        EXPECT_EQ(re.match("123abc$def"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("123abc$"sv, PosixFlags::Global).success, false);
    }
}

TEST_CASE(posix_basic_dollar_is_literal)
{
    // Ensure that a dollar sign in the middle is treated as a literal.
    {
        Regex<PosixBasic> re("abc$d");
        EXPECT_EQ(re.match("123abcdef"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("123abc"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("123abc$def"sv, PosixFlags::Global).success, true);
        EXPECT_EQ(re.match("123abc$"sv, PosixFlags::Global).success, false);
    }

    // Ensure that a dollar sign is always treated as a literal if escaped, even if at the end of the pattern.
    {
        Regex<PosixBasic> re("abc\\$");
        EXPECT_EQ(re.match("123abcdef"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("123abc"sv, PosixFlags::Global).success, false);
        EXPECT_EQ(re.match("123abc$def"sv, PosixFlags::Global).success, true);
        EXPECT_EQ(re.match("123abc$"sv, PosixFlags::Global).success, true);
    }
}

TEST_CASE(negative_lookahead)
{
    {
        // Negative lookahead with more than 2 forks difference between lookahead init and finish.
        auto options = ECMAScriptOptions { ECMAScriptFlags::Global };
        options.reset_flag((ECMAScriptFlags)regex::AllFlags::Internal_Stateful);
        Regex<ECMA262> re(":(?!\\^\\)|1)", options);
        EXPECT_EQ(re.match(":^)"sv).success, false);
        EXPECT_EQ(re.match(":1"sv).success, false);
        EXPECT_EQ(re.match(":foobar"sv).success, true);
    }
    {
        // Correctly count forks with nested groups and optimised loops
        Regex<ECMA262> re("^((?:[^\\n]|\\n(?! *\\n))+)(?:\\n *)+\\n");
        EXPECT_EQ(re.match("foo\n\n"sv).success, true);
        EXPECT_EQ(re.match("foo\n"sv).success, false);
    }
}

TEST_CASE(single_match_flag)
{
    {
        // Ensure that only a single match is produced and nothing past that.
        Regex<ECMA262> re("[\\u0008-\\uffff]"sv, ECMAScriptFlags::Global | (ECMAScriptFlags)regex::AllFlags::SingleMatch);
        auto result = re.match("ABC"sv);
        EXPECT_EQ(result.success, true);
        EXPECT_EQ(result.matches.size(), 1u);
        EXPECT_EQ(result.matches.first().view.to_byte_string(), "A"sv);
    }
}

TEST_CASE(empty_string_wildcard_match)
{
    {
        // Ensure that the wildcard ".*" matches the empty string exactly once
        Regex<ECMA262> re(".*"sv, ECMAScriptFlags::Global);
        auto result = re.match(""sv);
        EXPECT_EQ(result.success, true);
        EXPECT_EQ(result.matches.size(), 1u);
        EXPECT_EQ(result.matches.first().view.to_byte_string(), ""sv);
    }
}

TEST_CASE(inversion_state_in_char_class)
{
    {
        // #13755, /[\S\s]/.exec("hello") should be [ "h" ], not null.
        Regex<ECMA262> re("[\\S\\s]", ECMAScriptFlags::Global | (ECMAScriptFlags)regex::AllFlags::SingleMatch);

        auto result = re.match("hello"sv);
        EXPECT_EQ(result.success, true);
        EXPECT_EQ(result.matches.size(), 1u);
        EXPECT_EQ(result.matches.first().view.to_byte_string(), "h"sv);
    }
    {
        Regex<ECMA262> re("^(?:([^\\s!\"#%-,\\./;->@\\[-\\^`\\{-~]+(?=([=~}\\s/.)|]))))"sv, ECMAScriptFlags::Global);

        auto result = re.match("slideNumbers}}"sv);
        EXPECT_EQ(result.success, true);
        EXPECT_EQ(result.matches.size(), 1u);
        EXPECT_EQ(result.matches.first().view.to_byte_string(), "slideNumbers"sv);
        EXPECT_EQ(result.capture_group_matches.first()[0].view.to_byte_string(), "slideNumbers"sv);
        EXPECT_EQ(result.capture_group_matches.first()[1].view.to_byte_string(), "}"sv);
    }
    {
        // #21786, /[^\S\n]/.exec("\n") should be null, not [ "\n" ].
        // This was a general confusion between the inversion state and the negation state (temp inverse).
        Regex<ECMA262> re("[^\\S\\n]", ECMAScriptFlags::Global | (ECMAScriptFlags)regex::AllFlags::SingleMatch);

        auto result = re.match("\n"sv);
        EXPECT_EQ(result.success, false);
    }
}

TEST_CASE(mismatching_brackets)
{
    auto const test_cases = Array {
        "["sv,
        "[ -"sv,
    };

    for (auto const& test_case : test_cases) {
        Regex<ECMA262> re(test_case);
        EXPECT_EQ(re.parser_result.error, regex::Error::MismatchingBracket);
    }
}
