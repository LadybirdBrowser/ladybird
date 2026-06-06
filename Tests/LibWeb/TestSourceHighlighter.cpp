/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/SourceHighlighter.h>

static String highlight_source(String const& source, Syntax::Language language)
{
    auto url = URL::Parser::basic_parse("https://example.com/source.html"sv).release_value();
    auto base_url = URL::Parser::basic_parse("https://example.com/base/"sv).release_value();
    return WebView::highlight_source(url, base_url, source, language);
}

TEST_CASE(highlight_script_with_braces)
{
    // Regression test for https://github.com/LadybirdBrowser/ladybird/issues/8529
    auto source = "<script>\nfunction foo() {\n    return 1;\n}\n</script>"_string;
    auto result = highlight_source(source, Syntax::Language::HTML);
    EXPECT(!result.is_empty());
}

TEST_CASE(highlight_html_with_non_ascii_before_linkified_attribute)
{
    auto source = "<p title=\"\xF0\x9F\x98\x80\"><a href=\"next.html\">ok</a></p>"_string;
    auto result = highlight_source(source, Syntax::Language::HTML);

    EXPECT(result.contains("\xF0\x9F\x98\x80"sv));
    EXPECT(result.contains("<span class=\"attribute-name\">href</span>"sv));
    EXPECT(result.contains("<a href=\"https://example.com/base/next.html\"><span class=\"attribute-value\">\"next.html\"</span></a>"sv));
}

TEST_CASE(highlight_css_with_non_ascii_before_token)
{
    auto source = ".caf\xC3\xA9 { color: red; }"_string;
    auto result = highlight_source(source, Syntax::Language::CSS);

    EXPECT(result.contains("<span class=\"delimiter\">.</span><span class=\"identifier\">caf\xC3\xA9</span>"sv));
    EXPECT(result.contains("<span class=\"identifier\">color</span>"sv));
}

TEST_CASE(highlight_javascript_with_non_bmp_before_token)
{
    auto source = "const smile = \"\xF0\x9F\x98\x80\"; const answer = 42;"_string;
    auto result = highlight_source(source, Syntax::Language::JavaScript);

    EXPECT(result.contains("\xF0\x9F\x98\x80"sv));
    EXPECT(result.contains("<span class=\"identifier\">answer</span>"sv));
    EXPECT(result.contains("<span class=\"number\">42</span>"sv));
}

TEST_CASE(declares_utf8_and_preserves_non_ascii_text)
{
    auto source = "<style>:root { --label-text: \"Caf\xC3\xA9 cr\xC3\xA8me - \xE6\x9D\xB1\xE4\xBA\xAC - \xF0\x9F\x98\x80\"; }</style>"_string;
    auto result = highlight_source(source, Syntax::Language::HTML);

    EXPECT(result.contains("<meta charset=\"utf-8\">"sv));
    EXPECT(result.contains("Caf\xC3\xA9 cr\xC3\xA8me - \xE6\x9D\xB1\xE4\xBA\xAC - \xF0\x9F\x98\x80"sv));
    EXPECT(!result.contains("Caf\xC3\x83\xC2\xA9"sv));
}
