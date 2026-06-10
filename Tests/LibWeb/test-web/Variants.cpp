/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Variants.h"

#include <AK/GenericLexer.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>

namespace TestWeb {

static bool is_html_space(char ch)
{
    return ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r' || ch == ' ';
}

static bool is_htmlish_text_test(Test const& test)
{
    if (test.mode != TestMode::Text)
        return false;

    auto lexical_path = LexicalPath(test.input_path);
    auto extension = lexical_path.extension();
    return extension == "htm"sv || extension == "html"sv || extension == "xht"sv || extension == "xhtml"sv;
}

static void skip_to_end_of_tag(GenericLexer& lexer)
{
    while (!lexer.is_eof()) {
        auto ch = lexer.consume();
        if (ch == '"' || ch == '\'') {
            lexer.ignore_until(ch);
            lexer.consume_specific(ch);
            continue;
        }

        if (ch == '>')
            return;
    }
}

static void skip_until_case_insensitive(GenericLexer& lexer, StringView needle)
{
    while (!lexer.is_eof()) {
        auto next = lexer.peek_string(needle.length());
        if (next.has_value() && next->equals_ignoring_ascii_case(needle)) {
            lexer.ignore(needle.length());
            return;
        }

        lexer.ignore();
    }
}

static StringView consume_html_attribute_value(GenericLexer& lexer)
{
    lexer.ignore_while(is_html_space);

    if (lexer.next_is('"') || lexer.next_is('\'')) {
        auto quote = lexer.consume();
        auto value = lexer.consume_until(quote);
        lexer.consume_specific(quote);
        return value;
    }

    return lexer.consume_until([](auto ch) {
        return is_html_space(ch) || ch == '>';
    });
}

static ErrorOr<Vector<String>> read_static_test_variants(Test const& test)
{
    Vector<String> variants;
    if (!is_htmlish_text_test(test))
        return variants;

    auto file = TRY(Core::File::open(test.input_path, Core::File::OpenMode::Read));
    auto contents = TRY(file->read_until_eof());

    GenericLexer lexer { StringView { contents } };
    while (!lexer.is_eof()) {
        lexer.ignore_until('<');
        if (!lexer.consume_specific('<'))
            break;

        if (lexer.consume_specific("!--"sv)) {
            skip_until_case_insensitive(lexer, "-->"sv);
            continue;
        }

        lexer.ignore_while(is_html_space);

        if (lexer.consume_specific('/') || lexer.consume_specific('!') || lexer.consume_specific('?')) {
            skip_to_end_of_tag(lexer);
            continue;
        }

        auto tag_name = lexer.consume_until([](auto ch) {
            return is_html_space(ch) || ch == '/' || ch == '>';
        });

        if (tag_name.equals_ignoring_ascii_case("script"sv)) {
            skip_to_end_of_tag(lexer);
            skip_until_case_insensitive(lexer, "</script"sv);
            skip_to_end_of_tag(lexer);
            continue;
        }

        if (tag_name.equals_ignoring_ascii_case("style"sv)) {
            skip_to_end_of_tag(lexer);
            skip_until_case_insensitive(lexer, "</style"sv);
            skip_to_end_of_tag(lexer);
            continue;
        }

        if (!tag_name.equals_ignoring_ascii_case("meta"sv)) {
            skip_to_end_of_tag(lexer);
            continue;
        }

        Optional<StringView> name_attribute;
        Optional<StringView> content_attribute;

        while (!lexer.is_eof()) {
            lexer.ignore_while(is_html_space);

            if (lexer.consume_specific('>'))
                break;
            if (lexer.consume_specific('/'))
                continue;

            auto attribute_name = lexer.consume_until([](auto ch) {
                return is_html_space(ch) || ch == '=' || ch == '/' || ch == '>';
            });
            if (attribute_name.is_empty()) {
                lexer.ignore();
                continue;
            }

            lexer.ignore_while(is_html_space);

            StringView attribute_value = ""sv;
            if (lexer.consume_specific('='))
                attribute_value = consume_html_attribute_value(lexer);

            if (attribute_name.equals_ignoring_ascii_case("name"sv))
                name_attribute = attribute_value;
            else if (attribute_name.equals_ignoring_ascii_case("content"sv))
                content_attribute = attribute_value;
        }

        if (name_attribute.has_value() && name_attribute->equals_ignoring_ascii_case("variant"sv) && content_attribute.has_value()) {
            // Only query (?...) variants are expandable; skip anything else
            if (content_attribute->starts_with('?'))
                variants.append(TRY(String::from_utf8(*content_attribute)));
            else
                warnln("{}: ignoring unsupported variant content '{}'", test.input_path, *content_attribute);
        }
    }

    return variants;
}

void apply_variant_to_test(Test& test, String variant)
{
    VERIFY(variant.starts_with('?'));

    test.variant = move(variant);

    // relative_path uses '?' for display, safe_relative_path uses '@' for filesystem.
    auto variant_suffix = test.variant->bytes_as_string_view().substring_view(1);
    test.relative_path = ByteString::formatted("{}?{}", test.relative_path, variant_suffix);
    test.safe_relative_path = ByteString::formatted("{}@{}", test.safe_relative_path, variant_suffix);

    // Expected file: test@variant_suffix.txt
    auto dir = LexicalPath::dirname(test.expectation_path);
    auto title = LexicalPath::title(LexicalPath::basename(test.input_path));
    if (dir.is_empty())
        test.expectation_path = ByteString::formatted("{}@{}.txt", title, variant_suffix);
    else
        test.expectation_path = ByteString::formatted("{}/{}@{}.txt", dir, title, variant_suffix);
}

// https://web-platform-tests.org/writing-tests/testharness.html#variants
ErrorOr<void> expand_tests_with_static_variants(Vector<Test>& tests, Vector<ByteString> const& skipped_tests)
{
    Vector<Test> expanded_tests;
    expanded_tests.ensure_capacity(tests.size());

    for (auto& test : tests) {
        if (test.variant.has_value() || skipped_tests.contains_slow(test.input_path)) {
            expanded_tests.append(move(test));
            continue;
        }

        auto variants = TRY(read_static_test_variants(test));
        if (variants.is_empty()) {
            expanded_tests.append(move(test));
            continue;
        }

        expanded_tests.ensure_capacity(expanded_tests.size() + variants.size());
        for (auto const& variant : variants) {
            auto variant_test = test;
            apply_variant_to_test(variant_test, variant);
            expanded_tests.append(move(variant_test));
        }
    }

    tests = move(expanded_tests);
    return {};
}

}
