/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/NavigationLoader.h>

static Web::HTML::NavigationParamsDescriptor navigation_params_for(URL::URL const& url)
{
    return {
        .id = {},
        .navigable_id = {},
        .request = {},
        .response = {},
        .coop_enforcement_result = { .url = url, .origin = url.origin(), .opener_policy = {} },
        .reserved_environment = {},
        .origin = url.origin(),
        .policy_container = {},
        .opener_policy = {},
        .about_base_url = {},
    };
}

TEST_CASE(response_document_uses_history_entry_url_when_response_url_list_is_empty)
{
    auto url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = url;
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, move(request));
    loader->did_finish_navigation_params_creation({
        .navigation_params = navigation_params_for(url),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    });

    auto document = loader->response_document();
    EXPECT(document.has_value());
    if (document.has_value())
        EXPECT_EQ(document->url, url);
}

TEST_CASE(response_document_uses_last_response_url_after_redirects)
{
    auto initial_url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    auto final_url = URL::Parser::basic_parse("https://example.com/"sv).release_value();
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = initial_url;
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, move(request));
    auto params = navigation_params_for(initial_url);
    params.response.url_list = { initial_url, final_url };
    loader->did_finish_navigation_params_creation({
        .navigation_params = move(params),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    });

    auto document = loader->response_document();
    EXPECT(document.has_value());
    if (document.has_value())
        EXPECT_EQ(document->url, final_url);
}
