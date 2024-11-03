/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <regex.h>
#include <stdio.h>

TEST_CASE(catch_all)
{
    ByteString pattern = "^.*$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hello World", 0, NULL, 0), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(simple_start)
{
    ByteString pattern = "^hello friends";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hello!", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello friends", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Well, hello friends", 0, NULL, 0), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(simple_end)
{
    ByteString pattern = ".*hello\\.\\.\\. there$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hallo", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "I said fyhello... there", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "ahello... therea", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello.. there", 0, NULL, 0), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(simple_period)
{
    ByteString pattern = "hello.";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hello1", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello1", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "hello2", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "hello?", 0, NULL, 0), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(simple_period_end)
{
    ByteString pattern = "hello.$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED | REG_NOSUB), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hello1", 0, NULL, REG_NOSUB), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello1hello1", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "hello2hell", 0, NULL, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello?", 0, NULL, REG_NOSUB), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(simple_escaped)
{
    ByteString pattern = "hello\\.";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "hello", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello.", 0, NULL, 0), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(simple_period2_end)
{
    ByteString pattern = ".*hi... there$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hello there", 0, NULL, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "I said fyhi... there", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "....hi... ", 0, NULL, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "I said fyhihii there", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "I said fyhihi there", 0, NULL, REG_GLOBAL), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(simple_plus)
{
    ByteString pattern = "a+";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED | REG_NOSUB), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "b", 0, NULL, REG_NOSUB), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "a", 0, NULL, REG_NOSUB), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "aaaaaabbbbb", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "aaaaaaaaaaa", 0, NULL, REG_GLOBAL), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(simple_questionmark)
{
    ByteString pattern = "da?d";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a", 0, NULL, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "daa", 0, NULL, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "ddddd", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "dd", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "dad", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "dada", 0, NULL, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "adadaa", 0, NULL, REG_GLOBAL), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(simple_questionmark_matchall)
{
    ByteString pattern = "da?d";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a", num_matches, matches, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);
    EXPECT_EQ(regexec(&regex, "daa", num_matches, matches, REG_GLOBAL), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);

    EXPECT_EQ(regexec(&regex, "ddddd", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 2);

    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 2);
    EXPECT_EQ(matches[1].rm_so, 2);
    EXPECT_EQ(matches[1].rm_eo, 4);

    EXPECT_EQ(regexec(&regex, "dd", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(regexec(&regex, "dad", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(regexec(&regex, "dada", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(regexec(&regex, "adadaa", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);

    regfree(&regex);
}

TEST_CASE(character_class)
{
    ByteString pattern = "[[:alpha:]]";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    ByteString haystack = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, haystack.characters(), num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);
    EXPECT_EQ(regexec(&regex, haystack.characters(), num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 24);
    EXPECT_EQ(haystack.substring_view(matches[0].rm_so, matches[0].rm_eo - matches[0].rm_so), "W");
    EXPECT_EQ(haystack.substring_view(matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so), "i");

    regfree(&regex);
}

TEST_CASE(character_class2)
{
    ByteString pattern = "[[:alpha:]]*=([[:digit:]]*)|\\[(.*)\\]";
    regex_t regex;
    static constexpr int num_matches { 9 };
    regmatch_t matches[num_matches];

    ByteString haystack = "[Window]\nOpacity=255\nAudibleBeep=0\n";
    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED | REG_NEWLINE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, haystack.characters(), num_matches, matches, 0), REG_NOERR);

    EXPECT_EQ(matches[0].rm_cnt, 3);
#if 0
    for (int i = 0; i < num_matches; ++i) {
        fprintf(stderr, "Matches[%i].rm_so: %li, .rm_eo: %li .rm_cnt: %li: ", i, matches[i].rm_so, matches[i].rm_eo, matches[i].rm_cnt);
        fprintf(stderr, "haystack length: %lu\n", haystack.length());
        if (matches[i].rm_so != -1)
            fprintf(stderr, "%s\n", haystack.substring_view(matches[i].rm_so, matches[i].rm_eo - matches[i].rm_so).to_byte_string().characters());
    }
#endif

    EXPECT_EQ(haystack.substring_view(matches[0].rm_so, matches[0].rm_eo - matches[0].rm_so), "[Window]");

    EXPECT_EQ(matches[1].rm_so, -1);
    EXPECT_EQ(matches[1].rm_eo, -1);
    EXPECT_EQ(matches[1].rm_cnt, 0);

    EXPECT_EQ(haystack.substring_view(matches[2].rm_so, matches[2].rm_eo - matches[2].rm_so), "Window");
    EXPECT_EQ(haystack.substring_view(matches[3].rm_so, matches[3].rm_eo - matches[3].rm_so), "Opacity=255");

    EXPECT_EQ(haystack.substring_view(matches[4].rm_so, matches[4].rm_eo - matches[4].rm_so), "255");

    EXPECT_EQ(matches[5].rm_so, -1);
    EXPECT_EQ(matches[5].rm_eo, -1);
    EXPECT_EQ(matches[5].rm_cnt, 0);

    EXPECT_EQ(haystack.substring_view(matches[6].rm_so, matches[6].rm_eo - matches[6].rm_so), "AudibleBeep=0");
    EXPECT_EQ(haystack.substring_view(matches[7].rm_so, matches[7].rm_eo - matches[7].rm_so), "0");

    EXPECT_EQ(matches[8].rm_so, -1);
    EXPECT_EQ(matches[8].rm_eo, -1);
    EXPECT_EQ(matches[8].rm_cnt, 0);

    regfree(&regex);
}

TEST_CASE(escaped_char_questionmark)
{
    ByteString pattern = "This\\.?And\\.?That";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "ThisAndThat", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "This.And.That", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "This And That", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "This..And..That", 0, NULL, 0), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(char_qualifier_asterisk)
{
    ByteString pattern = "regex*";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "#include <regex.h>", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);

    regfree(&regex);
}

TEST_CASE(char_utf8)
{
    ByteString pattern = "😀";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Привет, мир! 😀 γειά σου κόσμος 😀 こんにちは世界", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 2);

    regfree(&regex);
}

TEST_CASE(parens)
{
    ByteString pattern = "test(hello)test";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "testhellotest", num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);

    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 13);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 9);

    regfree(&regex);
}

TEST_CASE(parser_error_parens)
{
    ByteString pattern = "test()test";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_EMPTY_EXPR);
    EXPECT_EQ(regexec(&regex, "testhellotest", num_matches, matches, 0), REG_EMPTY_EXPR);

    regfree(&regex);
}

TEST_CASE(parser_error_special_characters_used_at_wrong_place)
{
    ByteString pattern;
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    Vector<char, 4> chars = { '*', '+', '?', '}' };
    StringBuilder b;

    for (auto& ch : chars) {

        auto error_code_to_check = ch == '}' ? REG_EBRACE : REG_BADRPT;

        // First in ere
        b.clear();
        b.append(ch);
        pattern = b.to_byte_string();
        EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), error_code_to_check);
        EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), error_code_to_check);
        regfree(&regex);

        // After vertical line
        b.clear();
        b.append("a|"sv);
        b.append(ch);
        pattern = b.to_byte_string();
        EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), error_code_to_check);
        EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), error_code_to_check);
        regfree(&regex);

        // After circumflex
        b.clear();
        b.append('^');
        b.append(ch);
        pattern = b.to_byte_string();
        EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), error_code_to_check);
        EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), error_code_to_check);
        regfree(&regex);

        // After dollar
        b.clear();
        b.append('$');
        b.append(ch);
        pattern = b.to_byte_string();
        EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), error_code_to_check);
        EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), error_code_to_check);
        regfree(&regex);

        // After left parens
        b.clear();
        b.append('(');
        b.append(ch);
        b.append(')');
        pattern = b.to_byte_string();
        EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), error_code_to_check);
        EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), error_code_to_check);
        regfree(&regex);
    }
}

TEST_CASE(parser_error_vertical_line_used_at_wrong_place)
{
    ByteString pattern;
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    // First in ere
    pattern = "|asdf";
    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_EMPTY_EXPR);
    EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), REG_EMPTY_EXPR);
    regfree(&regex);

    // Last in ere
    pattern = "asdf|";
    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_EMPTY_EXPR);
    EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), REG_EMPTY_EXPR);
    regfree(&regex);

    // After left parens
    pattern = "(|asdf)";
    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_EMPTY_EXPR);
    EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), REG_EMPTY_EXPR);
    regfree(&regex);

    // Proceed right parens
    pattern = "(asdf)|";
    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_EMPTY_EXPR);
    EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), REG_EMPTY_EXPR);
    regfree(&regex);
}

TEST_CASE(parens_qualifier_questionmark)
{
    ByteString pattern = "test(hello)?test";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "testtest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 8);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testtest");

    match_str = "testhellotest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 13);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 9);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testhellotest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    regfree(&regex);
}

TEST_CASE(parens_qualifier_asterisk)
{
    ByteString pattern = "test(hello)*test";
    regex_t regex;
    static constexpr int num_matches { 6 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "testtest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 8);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testtest");

    match_str = "testhellohellotest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 18);
    EXPECT_EQ(matches[1].rm_so, 9);
    EXPECT_EQ(matches[1].rm_eo, 14);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testhellohellotest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "testhellohellotest, testhellotest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 2);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 18);
    EXPECT_EQ(matches[1].rm_so, 9);
    EXPECT_EQ(matches[1].rm_eo, 14);
    EXPECT_EQ(matches[2].rm_so, 20);
    EXPECT_EQ(matches[2].rm_eo, 33);
    EXPECT_EQ(matches[3].rm_so, 24);
    EXPECT_EQ(matches[3].rm_eo, 29);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testhellohellotest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "testhellotest");
    EXPECT_EQ(StringView(&match_str[matches[3].rm_so], matches[3].rm_eo - matches[3].rm_so), "hello");

    regfree(&regex);
}

TEST_CASE(parens_qualifier_asterisk_2)
{
    ByteString pattern = "test(.*)test";
    regex_t regex;
    static constexpr int num_matches { 6 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "testasdftest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 12);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 8);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testasdftest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "asdf");

    match_str = "testasdfasdftest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 16);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 12);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testasdfasdftest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "asdfasdf");

    match_str = "testaaaatest, testbbbtest, testtest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 35);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 31);

    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testaaaatest, testbbbtest, testtest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "aaaatest, testbbbtest, test");

    regfree(&regex);
}

TEST_CASE(mulit_parens_qualifier_too_less_result_values)
{
    ByteString pattern = "test(a)?(b)?(c)?test";
    regex_t regex;
    static constexpr int num_matches { 4 };
    regmatch_t matches[num_matches];
    char const* match_str;

    matches[3] = { -2, -2, 100 };

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "testabtest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches - 1, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 10);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 5);
    EXPECT_EQ(matches[2].rm_so, 5);
    EXPECT_EQ(matches[2].rm_eo, 6);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testabtest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "b");
    EXPECT_EQ(matches[3].rm_so, -2);
    EXPECT_EQ(matches[3].rm_eo, -2);
    EXPECT_EQ(matches[3].rm_cnt, 100);

    match_str = "testabctest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches - 1, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 11);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 5);
    EXPECT_EQ(matches[2].rm_so, 5);
    EXPECT_EQ(matches[2].rm_eo, 6);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testabctest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "b");
    EXPECT_EQ(matches[3].rm_so, -2);
    EXPECT_EQ(matches[3].rm_eo, -2);
    EXPECT_EQ(matches[3].rm_cnt, 100);

    match_str = "testabctest, testabctest";

    EXPECT_EQ(regexec(&regex, match_str, num_matches - 1, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 2);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 11);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 5);
    EXPECT_EQ(matches[2].rm_so, 5);
    EXPECT_EQ(matches[2].rm_eo, 6);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testabctest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "b");
    EXPECT_EQ(matches[3].rm_so, -2);
    EXPECT_EQ(matches[3].rm_eo, -2);
    EXPECT_EQ(matches[3].rm_cnt, 100);

    regfree(&regex);
}

TEST_CASE(multi_parens_qualifier_questionmark)
{
    ByteString pattern = "test(a)?(b)?(c)?test";
    regex_t regex;
    static constexpr int num_matches { 8 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "testtest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 8);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testtest");

    match_str = "testabctest";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 11);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 5);
    EXPECT_EQ(matches[2].rm_so, 5);
    EXPECT_EQ(matches[2].rm_eo, 6);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testabctest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "b");

    match_str = "testabctest, testactest";

    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 2);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 11);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 5);
    EXPECT_EQ(matches[2].rm_so, 5);
    EXPECT_EQ(matches[2].rm_eo, 6);
    EXPECT_EQ(matches[3].rm_so, 6);
    EXPECT_EQ(matches[3].rm_eo, 7);

    EXPECT_EQ(matches[4].rm_so, 13);
    EXPECT_EQ(matches[4].rm_eo, 23);
    EXPECT_EQ(matches[5].rm_so, 17);
    EXPECT_EQ(matches[5].rm_eo, 18);
    EXPECT_EQ(matches[6].rm_so, -1);
    EXPECT_EQ(matches[6].rm_eo, -1);
    EXPECT_EQ(matches[7].rm_so, 18);
    EXPECT_EQ(matches[7].rm_eo, 19);

    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testabctest");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "b");
    EXPECT_EQ(StringView(&match_str[matches[3].rm_so], matches[3].rm_eo - matches[3].rm_so), "c");
    EXPECT_EQ(StringView(&match_str[matches[4].rm_so], matches[4].rm_eo - matches[4].rm_so), "testactest");
    EXPECT_EQ(StringView(&match_str[matches[5].rm_so], matches[5].rm_eo - matches[5].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[6].rm_so], matches[6].rm_eo - matches[6].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[7].rm_so], matches[7].rm_eo - matches[7].rm_so), "c");

    regfree(&regex);
}

TEST_CASE(simple_alternative)
{
    ByteString pattern = "test|hello|friends";
    regex_t regex;
    static constexpr int num_matches { 1 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "test", num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 4);

    EXPECT_EQ(regexec(&regex, "hello", num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 5);

    EXPECT_EQ(regexec(&regex, "friends", num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 7);

    regfree(&regex);
}

TEST_CASE(alternative_match_groups)
{
    ByteString pattern = "test(a)?(b)?|hello ?(dear|my)? friends";
    regex_t regex;
    static constexpr int num_matches { 8 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "test";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 4);
    EXPECT_EQ(matches[1].rm_so, -1);
    EXPECT_EQ(matches[1].rm_eo, -1);
    EXPECT_EQ(matches[2].rm_so, -1);
    EXPECT_EQ(matches[2].rm_eo, -1);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "test");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "");

    match_str = "testa";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 5);
    EXPECT_EQ(matches[1].rm_so, 4);
    EXPECT_EQ(matches[1].rm_eo, 5);
    EXPECT_EQ(matches[2].rm_so, -1);
    EXPECT_EQ(matches[2].rm_eo, -1);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testa");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "a");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "");

    match_str = "testb";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 5);
    EXPECT_EQ(matches[1].rm_so, -1);
    EXPECT_EQ(matches[1].rm_eo, -1);
    EXPECT_EQ(matches[2].rm_so, 4);
    EXPECT_EQ(matches[2].rm_eo, 5);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "testb");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "b");

    match_str = "hello friends";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 13);
    EXPECT_EQ(matches[1].rm_so, -1);
    EXPECT_EQ(matches[1].rm_eo, -1);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hello friends");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "");

    match_str = "hello dear friends";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 18);
    EXPECT_EQ(matches[1].rm_so, -1);
    EXPECT_EQ(matches[1].rm_eo, -1);
    EXPECT_EQ(matches[2].rm_so, -1);
    EXPECT_EQ(matches[2].rm_eo, -1);
    EXPECT_EQ(matches[3].rm_so, 6);
    EXPECT_EQ(matches[3].rm_eo, 10);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hello dear friends");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[3].rm_so], matches[3].rm_eo - matches[3].rm_so), "dear");

    match_str = "hello my friends";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 16);
    EXPECT_EQ(matches[1].rm_so, -1);
    EXPECT_EQ(matches[1].rm_eo, -1);
    EXPECT_EQ(matches[2].rm_so, -1);
    EXPECT_EQ(matches[2].rm_eo, -1);
    EXPECT_EQ(matches[3].rm_so, 6);
    EXPECT_EQ(matches[3].rm_eo, 8);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hello my friends");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[2].rm_so], matches[2].rm_eo - matches[2].rm_so), "");
    EXPECT_EQ(StringView(&match_str[matches[3].rm_so], matches[3].rm_eo - matches[3].rm_so), "my");

    match_str = "testabc";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);
    EXPECT_EQ(matches[0].rm_so, -1);
    EXPECT_EQ(matches[0].rm_eo, -1);

    match_str = "hello test friends";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);
    EXPECT_EQ(matches[0].rm_so, -1);
    EXPECT_EQ(matches[0].rm_eo, -1);

    regfree(&regex);
}

TEST_CASE(parens_qualifier_exact)
{
    ByteString pattern = "(hello){3}";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "hello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);

    match_str = "hellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 15);
    EXPECT_EQ(matches[1].rm_so, 10);
    EXPECT_EQ(matches[1].rm_eo, 15);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "hellohellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 15);
    EXPECT_EQ(matches[1].rm_so, 10);
    EXPECT_EQ(matches[1].rm_eo, 15);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "test hellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 5);
    EXPECT_EQ(matches[0].rm_eo, 20);
    EXPECT_EQ(matches[1].rm_so, 15);
    EXPECT_EQ(matches[1].rm_eo, 20);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    regfree(&regex);
}

TEST_CASE(parens_qualifier_minimum)
{
    ByteString pattern = "(hello){3,}";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "hello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);

    match_str = "hellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 15);
    EXPECT_EQ(matches[1].rm_so, 10);
    EXPECT_EQ(matches[1].rm_eo, 15);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "hellohellohellohello";

    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_SEARCH), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 20);
    EXPECT_EQ(matches[1].rm_so, 15);
    EXPECT_EQ(matches[1].rm_eo, 20);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "test hellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 5);
    EXPECT_EQ(matches[0].rm_eo, 20);
    EXPECT_EQ(matches[1].rm_so, 15);
    EXPECT_EQ(matches[1].rm_eo, 20);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "test hellohellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 5);
    EXPECT_EQ(matches[0].rm_eo, 25);
    EXPECT_EQ(matches[1].rm_so, 20);
    EXPECT_EQ(matches[1].rm_eo, 25);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    regfree(&regex);
}

TEST_CASE(parens_qualifier_maximum)
{
    ByteString pattern = "(hello){2,3}";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];
    char const* match_str;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    match_str = "hello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(matches[0].rm_cnt, 0);

    match_str = "hellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 15);
    EXPECT_EQ(matches[1].rm_so, 10);
    EXPECT_EQ(matches[1].rm_eo, 15);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "hellohellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 0);
    EXPECT_EQ(matches[0].rm_eo, 15);
    EXPECT_EQ(matches[1].rm_so, 10);
    EXPECT_EQ(matches[1].rm_eo, 15);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "test hellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 5);
    EXPECT_EQ(matches[0].rm_eo, 20);
    EXPECT_EQ(matches[1].rm_so, 15);
    EXPECT_EQ(matches[1].rm_eo, 20);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    match_str = "test hellohellohellohello";
    EXPECT_EQ(regexec(&regex, match_str, num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(matches[0].rm_so, 5);
    EXPECT_EQ(matches[0].rm_eo, 20);
    EXPECT_EQ(matches[1].rm_so, 15);
    EXPECT_EQ(matches[1].rm_eo, 20);
    EXPECT_EQ(StringView(&match_str[matches[0].rm_so], matches[0].rm_eo - matches[0].rm_so), "hellohellohello");
    EXPECT_EQ(StringView(&match_str[matches[1].rm_so], matches[1].rm_eo - matches[1].rm_so), "hello");

    regfree(&regex);
}

TEST_CASE(char_qualifier_min_max)
{
    ByteString pattern = "c{3,30}";
    regex_t regex;
    static constexpr int num_matches { 5 };
    regmatch_t matches[num_matches];

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "cc", num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "ccc", num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "cccccccccccccccccccccccccccccc", num_matches, matches, 0), REG_NOERR);
    EXPECT_EQ(matches[0].rm_cnt, 1);
    EXPECT_EQ(regexec(&regex, "ccccccccccccccccccccccccccccccc", num_matches, matches, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "ccccccccccccccccccccccccccccccc", num_matches, matches, REG_GLOBAL), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "cccccccccccccccccccccccccccccccc", num_matches, matches, 0), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(simple_bracket_chars)
{
    ByteString pattern = "[abc]";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "b", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "c", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "d", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "e", 0, NULL, 0), REG_NOMATCH);
    regfree(&regex);
}

TEST_CASE(simple_bracket_chars_inverse)
{
    ByteString pattern = "[^abc]";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "b", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "c", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "d", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "e", 0, NULL, 0), REG_NOERR);
    regfree(&regex);
}

TEST_CASE(simple_bracket_chars_range)
{
    ByteString pattern = "[a-d]";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "b", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "c", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "d", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "e", 0, NULL, 0), REG_NOMATCH);
    regfree(&regex);
}

TEST_CASE(simple_bracket_chars_range_inverse)
{
    ByteString pattern = "[^a-df-z]";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "b", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "c", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "d", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "e", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "k", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "z", 0, NULL, 0), REG_NOMATCH);
    regfree(&regex);
}

TEST_CASE(bracket_character_class_uuid)
{
    ByteString pattern = "^([[:xdigit:]]{8})-([[:xdigit:]]{4})-([[:xdigit:]]{4})-([[:xdigit:]]{4})-([[:xdigit:]]{12})$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "fb9b62a2-1579-4e3a-afba-76239ccb6583", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "fb9b62a2", 0, NULL, 0), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(simple_bracket_character_class_inverse)
{
    ByteString pattern = "[^[:digit:]]";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "1", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "2", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "3", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "d", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "e", 0, NULL, 0), REG_NOERR);
    regfree(&regex);
}

TEST_CASE(email_address)
{
    ByteString pattern = "^[A-Z0-9a-z._%+-]{1,64}@(?:[A-Za-z0-9-]{1,63}\\.){1,125}[A-Za-z]{2,63}$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "emanuel.sprung@gmail.com", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "andreas@ladybird.org", 0, NULL, 0), REG_NOERR);

    regfree(&regex);
}

TEST_CASE(error_message)
{
    ByteString pattern = "^[A-Z0-9[a-z._%+-]{1,64}@[A-Za-z0-9-]{1,63}\\.{1,125}[A-Za-z]{2,63}$";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED), REG_EBRACK);
    EXPECT_EQ(regexec(&regex, "asdf@asdf.com", 0, NULL, 0), REG_EBRACK);
    char buf[1024];
    size_t buflen = 1024;
    auto len = regerror(0, &regex, buf, buflen);
    ByteString expected = "Error during parsing of regular expression:\n    ^[A-Z0-9[a-z._%+-]{1,64}@[A-Za-z0-9-]{1,63}\\.{1,125}[A-Za-z]{2,63}$\n             ^---- [ ] imbalance.";
    for (size_t i = 0; i < len; ++i) {
        EXPECT_EQ(buf[i], expected[i]);
    }

    regfree(&regex);
}

TEST_CASE(simple_ignorecase)
{
    ByteString pattern = "^hello friends";
    regex_t regex;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED | REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "Hello Friends", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "hello Friends", 0, NULL, 0), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "hello Friends!", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello Friends!", 0, NULL, REG_GLOBAL), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "hell Friends", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hell Friends", 0, NULL, REG_GLOBAL), REG_NOMATCH);

    regfree(&regex);
}

TEST_CASE(simple_notbol_noteol)
{
    ByteString pattern = "^hello friends$";
    ByteString pattern2 = "hello friends";
    regex_t regex, regex2;

    EXPECT_EQ(regcomp(&regex, pattern.characters(), REG_EXTENDED | REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regcomp(&regex2, pattern2.characters(), REG_EXTENDED | REG_NOSUB | REG_ICASE), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "hello friends", 0, NULL, REG_NOTBOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello friends", 0, NULL, REG_NOTEOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello friends", 0, NULL, REG_NOTBOL | REG_NOTEOL), REG_NOMATCH);

    EXPECT_EQ(regexec(&regex, "a hello friends b", 0, NULL, REG_NOTBOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "a hello friends", 0, NULL, REG_NOTBOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "a hello friends", 0, NULL, REG_NOTBOL | REG_SEARCH), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a hello friends b", 0, NULL, REG_NOTBOL | REG_SEARCH), REG_NOERR);

    EXPECT_EQ(regexec(&regex, "a hello friends b", 0, NULL, REG_NOTEOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello friends b", 0, NULL, REG_NOTEOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "hello friends b", 0, NULL, REG_NOTEOL | REG_SEARCH), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "a hello friends b", 0, NULL, REG_NOTEOL | REG_SEARCH), REG_NOMATCH);

    EXPECT_EQ(regexec(&regex, "a hello friends b", 0, NULL, REG_NOTBOL | REG_NOTEOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "a hello friends b", 0, NULL, REG_NOTBOL | REG_NOTEOL | REG_SEARCH), REG_NOMATCH);

    EXPECT_EQ(regexec(&regex2, "hello friends", 0, NULL, REG_NOTBOL), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex2, "hello friends", 0, NULL, REG_NOTEOL), REG_NOMATCH);

    regfree(&regex);
    regfree(&regex2);
}

TEST_CASE(bre_basic)
{
    regex_t regex;
    EXPECT_EQ(regcomp(&regex, "hello friends", REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "hello friends", 0, NULL, 0), REG_NOERR);
    regfree(&regex);

    EXPECT_EQ(regcomp(&regex, "\\(15\\)\\1", REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "1515", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "55", 0, NULL, 0), REG_NOMATCH);
    regfree(&regex);

    EXPECT_EQ(regcomp(&regex, "15\\{1,2\\}", REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "15", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "1515", 0, NULL, 0), REG_NOMATCH);
    EXPECT_EQ(regexec(&regex, "55", 0, NULL, 0), REG_NOMATCH);
    regfree(&regex);

    EXPECT_EQ(regcomp(&regex, "15{1,2}", REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "15{1,2}", 0, NULL, 0), REG_NOERR);
    regfree(&regex);

    EXPECT_EQ(regcomp(&regex, "1[56]", REG_NOSUB | REG_ICASE), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "15", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "16", 0, NULL, 0), REG_NOERR);
    EXPECT_EQ(regexec(&regex, "17", 0, NULL, 0), REG_NOMATCH);
    regfree(&regex);
}
