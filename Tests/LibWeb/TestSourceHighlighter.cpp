/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <LibWebView/SourceHighlighter.h>

TEST_CASE(highlight_script_with_braces)
{
    // Regression test for https://github.com/LadybirdBrowser/ladybird/issues/8529
    auto source = "<script>\nfunction foo() {\n    return 1;\n}\n</script>"_string;
    URL::URL base_url {};
    auto result = WebView::highlight_source({}, base_url, source, Syntax::Language::HTML, WebView::HighlightOutputMode::SourceOnly);
    EXPECT(!result.is_empty());
}
