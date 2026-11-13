/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>
#include <LibWebView/SiteCompatibility.h>

static ErrorOr<JsonValue> load_data_from_file(StringView contents)
{
    Test::TemporaryDirectory directory { "TestSiteCompatibilityDataLoading"sv };
    auto path = directory.path().append("WebCompat.json"sv).string();
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(contents.bytes()));
    return WebView::load_site_compatibility_data(TRY(String::formatted("file://{}", path)));
}

TEST_CASE(missing_resource_file_is_rejected)
{
    auto result = WebView::load_site_compatibility_data("file:///__ladybird_missing_web_compat_file__.json"sv);
    EXPECT(result.is_error());
}

TEST_CASE(loads_multiple_rules_from_one_file)
{
    auto json = TRY_OR_FAIL(load_data_from_file(R"([
        {
            "sites": ["https://first.example"],
            "user_agent": ["hide_Ladybird"]
        },
        {
            "sites": ["https://second.example"],
            "user_agent": ["hide_Ladybird", "hide_Chromium"],
            "expose_experimental_interfaces": ["AudioContext"]
        }
    ])"sv));
    auto data = TRY_OR_FAIL(Web::SiteCompatibilityData::from_json(json));
    auto first_origin = URL::Parser::basic_parse("https://first.example/"sv).release_value().origin();
    auto second_origin = URL::Parser::basic_parse("https://second.example/"sv).release_value().origin();
    constexpr auto user_agent = "Mozilla/5.0 Ladybird/1.0 Chrome/153.0.0.0"sv;

    EXPECT_EQ(data.rules().size(), 2u);
    EXPECT(data.rules()[0].matches(URL::Site::obtain(first_origin)));
    EXPECT(data.rules()[1].matches(URL::Site::obtain(second_origin)));
    EXPECT_EQ(data.user_agent_for(first_origin, first_origin, user_agent), "Mozilla/5.0 Chrome/153.0.0.0"sv);
    EXPECT_EQ(data.user_agent_for(second_origin, second_origin, user_agent), "Mozilla/5.0"sv);
    EXPECT(data.exposes_experimental_interface(second_origin, second_origin, "AudioContext"sv));
}

TEST_CASE(invalid_file_contents_are_rejected)
{
    EXPECT(load_data_from_file("not json"sv).is_error());
    EXPECT(load_data_from_file(R"({"sites": ["https://example.com"], "user_agent": ["hide_Ladybird"]})"sv).is_error());
    EXPECT(load_data_from_file(R"([{"sites": ["https://example.com"], "user_agent": ["unknown"]}])"sv).is_error());
}
