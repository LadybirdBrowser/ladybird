/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Loader/SiteCompatibility.h>

static constexpr auto default_user_agent = "Mozilla/5.0 Ladybird/1.0 Chrome/146.0.0.0 Safari/537.36"sv;

static URL::URL url(StringView input)
{
    return URL::Parser::basic_parse(input).release_value();
}

TEST_CASE(rule_matches_url_patterns)
{
    auto rule = MUST(Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        {
            "matches": ["https://www.example.com/*"],
            "user_agent": ["hide_Ladybird"]
        }
    )"sv))));

    EXPECT(rule.matches(url("https://www.example.com/article"sv)));
    EXPECT(!rule.matches(url("http://www.example.com/article"sv)));
    EXPECT(!rule.matches(url("https://example.com/article"sv)));
    EXPECT(!rule.matches(url("https://www.example.com.evil.test/article"sv)));
}

TEST_CASE(rule_matches_apex_and_subdomain_patterns)
{
    auto rule = MUST(Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        {
            "matches": ["https://example.com/*", "https://*.example.com/*"],
            "user_agent": ["hide_Ladybird"]
        }
    )"sv))));

    EXPECT(rule.matches(url("https://example.com/article"sv)));
    EXPECT(rule.matches(url("https://www.example.com/article"sv)));
    EXPECT(rule.matches(url("https://edition.news.example.com/article"sv)));
    EXPECT(!rule.matches(url("https://example.com.evil.test/article"sv)));
    EXPECT(!rule.matches(url("https://notexample.com/article"sv)));
}

TEST_CASE(rule_transforms_user_agent)
{
    auto rule = MUST(Web::SiteCompatibilityRule::create(
        { "https://www.example.com/*"_string },
        { Web::UserAgentTransformation::HideLadybird }));

    EXPECT_EQ(rule.apply_user_agent_transformations(default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
}

TEST_CASE(data_only_transforms_matching_urls)
{
    Web::SiteCompatibilityData data;
    data.add_rule(MUST(Web::SiteCompatibilityRule::create(
        { "https://www.example.com/*"_string },
        { Web::UserAgentTransformation::HideLadybird })));

    EXPECT_EQ(data.user_agent_for_url(url("https://www.example.com/"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for_url(url("https://elsewhere.example/"sv), default_user_agent), default_user_agent);
}

TEST_CASE(data_maps_websocket_urls_to_http_for_matching)
{
    Web::SiteCompatibilityData data;
    data.add_rule(MUST(Web::SiteCompatibilityRule::create(
        { "http://www.example.com/*"_string, "https://secure.example.com/*"_string },
        { Web::UserAgentTransformation::HideLadybird })));

    EXPECT_EQ(data.user_agent_for_websocket_url(url("ws://www.example.com/socket"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for_websocket_url(url("wss://secure.example.com/socket"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for_websocket_url(url("ws://secure.example.com/socket"sv), default_user_agent), default_user_agent);
    EXPECT_EQ(data.user_agent_for_websocket_url(url("wss://www.example.com/socket"sv), default_user_agent), default_user_agent);
}

TEST_CASE(hide_ladybird_only_removes_ladybird_products)
{
    auto rule = MUST(Web::SiteCompatibilityRule::create(
        { "https://www.example.com/*"_string },
        { Web::UserAgentTransformation::HideLadybird }));

    EXPECT_EQ(rule.apply_user_agent_transformations("Ladybird/1.0"sv), String {});
    EXPECT_EQ(rule.apply_user_agent_transformations("Ladybird/1.0 Mozilla/5.0"sv), "Mozilla/5.0"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 Ladybird/1.0"sv), "Mozilla/5.0"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 Ladybird Preview/1.0"sv), "Mozilla/5.0 Ladybird Preview/1.0"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 MyLadybird/1.0"sv), "Mozilla/5.0 MyLadybird/1.0"_string);
}

TEST_CASE(data_parses_rule_array)
{
    auto data = MUST(Web::SiteCompatibilityData::from_json(MUST(JsonValue::from_string(R"(
        [{
            "matches": ["https://www.example.com/*"],
            "user_agent": ["hide_Ladybird"]
        }]
    )"sv))));

    EXPECT_EQ(data.rules().size(), 1u);
    EXPECT_EQ(data.user_agent_for_url(url("https://www.example.com/"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
}

TEST_CASE(invalid_rules_are_rejected)
{
    auto missing_matches = Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        { "user_agent": ["hide_Ladybird"] }
    )"sv)));
    EXPECT(missing_matches.is_error());

    auto unknown_transformation = Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        { "matches": ["https://example.com/*"], "user_agent": ["dance"] }
    )"sv)));
    EXPECT(unknown_transformation.is_error());

    auto invalid_pattern = Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        { "matches": ["["], "user_agent": ["hide_Ladybird"] }
    )"sv)));
    EXPECT(invalid_pattern.is_error());
}
