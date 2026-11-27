/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/WebDriver/Network.h>
#include <LibWebView/WebDriverBiDiNetwork.h>

TEST_CASE(scoped_network_state_only_contains_hosted_traversables)
{
    Web::WebDriver::BiDiSessionState state;
    state.is_active = true;
    state.network_default_headers.append({ "X-Default", "default" });
    state.network_user_context_headers.set("user"_string, { { "X-User", "user" } });
    state.network_navigable_headers.set("1-1"_string, { { "Authorization", "first-secret" } });
    state.network_navigable_headers.set("2-1"_string, { { "Authorization", "second-secret" } });
    state.network_navigable_headers.set("3-1"_string, { { "Authorization", "foreign-secret" } });
    state.network_navigable_cache_behavior.set("1-1"_string, "bypass"_string);
    state.network_navigable_cache_behavior.set("3-1"_string, "bypass"_string);

    HashTable<String> hosted;
    hosted.set("1-1"_string);
    hosted.set("2-1"_string);
    auto filtered = WebView::session_state_for_hosted_traversables(state, hosted);
    EXPECT(filtered.is_active);
    EXPECT_EQ(filtered.network_navigable_headers.size(), 2u);
    EXPECT(filtered.network_navigable_headers.contains("1-1"sv));
    EXPECT(filtered.network_navigable_headers.contains("2-1"sv));
    EXPECT(!filtered.network_navigable_headers.contains("3-1"sv));
    EXPECT_EQ(filtered.network_navigable_cache_behavior.size(), 1u);
    EXPECT(!filtered.network_navigable_cache_behavior.contains("3-1"sv));
    EXPECT_EQ(filtered.network_default_headers.size(), 1u);
    EXPECT_EQ(filtered.network_user_context_headers.size(), 1u);
    EXPECT_EQ(state.network_navigable_headers.size(), 3u);
    EXPECT_EQ(state.network_navigable_cache_behavior.size(), 2u);

    auto unhosted = WebView::session_state_for_hosted_traversables(state, {});
    EXPECT(unhosted.network_navigable_headers.is_empty());
    EXPECT(unhosted.network_navigable_cache_behavior.is_empty());
    EXPECT_EQ(unhosted.network_default_headers.size(), 1u);
}

TEST_CASE(a_network_event_ends_a_routed_block_only_for_the_interception_it_names)
{
    auto parameters = MUST(JsonValue::from_string(R"({"isBlocked":false,"__webdriver_network_interception":7})"sv)).as_object();
    EXPECT_EQ(WebView::take_network_interception(parameters), 7u);
    EXPECT(!parameters.has(Web::WebDriver::Network::network_interception_parameter));
    EXPECT(!WebView::take_network_interception(parameters).has_value());

    auto ended = Web::WebDriver::Network::network_interception_ended_event;
    EXPECT(WebView::network_event_ends_routed_block(ended, 7, 7));

    // The end of another fetch of the request, which shares its id, names no interception of the block.
    EXPECT(!WebView::network_event_ends_routed_block(ended, {}, 7));
    EXPECT(!WebView::network_event_ends_routed_block(ended, 8, 7));
    EXPECT(!WebView::network_event_ends_routed_block("network.fetchError"sv, 7, 7));
    EXPECT(!WebView::network_event_ends_routed_block("network.beforeRequestSent"sv, 7, 7));
}
