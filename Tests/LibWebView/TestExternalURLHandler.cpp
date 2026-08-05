/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/ExternalURLHandler.h>

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

TEST_CASE(denied_schemes)
{
    constexpr Array denied_schemes {
        "afp"sv,
        "data"sv,
        "disk"sv,
        "disks"sv,
        "file"sv,
        "hcp"sv,
        "ie.http"sv,
        "javascript"sv,
        "mk"sv,
        "ms-help"sv,
        "nntp"sv,
        "res"sv,
        "shell"sv,
        "vbscript"sv,
        "view-source"sv,
        "vnd.ms.radio"sv,
    };

    for (auto scheme : denied_schemes)
        EXPECT(WebView::ExternalURLRequestPolicy::is_denied_scheme(scheme));

    EXPECT(WebView::ExternalURLRequestPolicy::is_denied_scheme("c"sv));
    EXPECT(!WebView::ExternalURLRequestPolicy::is_denied_scheme("mailto"sv));
    EXPECT(!WebView::ExternalURLRequestPolicy::is_denied_scheme("web+test"sv));
}

TEST_CASE(mailto_with_activation_launches)
{
    WebView::ExternalURLRequestPolicy policy;
    policy.allow_next_request();
    EXPECT_EQ(policy.decide(parse_url("mailto:foo@example.com"sv), true), WebView::ExternalURLAction::Launch);
}

TEST_CASE(mailto_without_activation_prompts)
{
    WebView::ExternalURLRequestPolicy policy;
    policy.allow_next_request();
    EXPECT_EQ(policy.decide(parse_url("mailto:foo@example.com"sv), false), WebView::ExternalURLAction::Prompt);
}

TEST_CASE(mailto_from_browser_ui_launches)
{
    WebView::ExternalURLRequestPolicy policy;
    auto url = parse_url("mailto:foo@example.com"sv);
    policy.allow_request_from_browser_ui(url);
    EXPECT_EQ(policy.decide(parse_url("mailto:bar@example.com"sv), false), WebView::ExternalURLAction::Block);
    EXPECT_EQ(policy.decide(url, false), WebView::ExternalURLAction::Launch);
}

TEST_CASE(other_schemes_always_prompt)
{
    WebView::ExternalURLRequestPolicy policy;
    policy.allow_next_request();
    EXPECT_EQ(policy.decide(parse_url("web+test:payload"sv), true), WebView::ExternalURLAction::Prompt);

    EXPECT_EQ(policy.decide(parse_url("web+test:payload"sv), false), WebView::ExternalURLAction::Prompt);
}

TEST_CASE(requests_require_an_interaction)
{
    WebView::ExternalURLRequestPolicy policy;
    auto url = parse_url("mailto:foo@example.com"sv);

    EXPECT_EQ(policy.decide(url, true), WebView::ExternalURLAction::Block);

    policy.allow_next_request();
    EXPECT_EQ(policy.decide(url, true), WebView::ExternalURLAction::Launch);

    EXPECT(policy.take_request_allowance().has_value());
    EXPECT_EQ(policy.decide(url, true), WebView::ExternalURLAction::Block);
}

TEST_CASE(request_allowances_expire)
{
    WebView::ExternalURLRequestPolicy policy;
    auto url = parse_url("mailto:foo@example.com"sv);
    auto granted_at = MonotonicTime::now();

    policy.allow_next_request(granted_at);
    EXPECT_EQ(policy.decide(url, true, granted_at + AK::Duration::from_seconds(6)), WebView::ExternalURLAction::Block);
    EXPECT(!policy.take_request_allowance().has_value());
}

TEST_CASE(navigation_only_clears_page_allowances)
{
    WebView::ExternalURLRequestPolicy policy;
    auto url = parse_url("mailto:foo@example.com"sv);

    policy.allow_next_request();
    policy.clear_page_request_allowance();
    EXPECT_EQ(policy.decide(url, true), WebView::ExternalURLAction::Block);

    policy.allow_request_from_browser_ui(url);
    policy.clear_page_request_allowance();
    EXPECT_EQ(policy.decide(url, false), WebView::ExternalURLAction::Launch);
}

TEST_CASE(denied_schemes_do_not_consume_the_request_allowance)
{
    WebView::ExternalURLRequestPolicy policy;
    policy.allow_next_request();

    EXPECT_EQ(policy.decide(parse_url("file:///tmp/example"sv), true), WebView::ExternalURLAction::Block);
    EXPECT_EQ(policy.decide(parse_url("mailto:foo@example.com"sv), true), WebView::ExternalURLAction::Launch);
}
