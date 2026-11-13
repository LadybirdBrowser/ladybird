/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/JsonValue.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>

static constexpr auto default_user_agent = "Mozilla/5.0 Ladybird/1.0 Chrome/146.0.0.0 Safari/537.36"sv;

static URL::Origin origin(StringView input)
{
    return URL::Parser::basic_parse(input).release_value().origin();
}

static URL::Site site(StringView input)
{
    return URL::Site::obtain(origin(input));
}

TEST_CASE(rule_matches_its_sites)
{
    auto rule = MUST(Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        {
            "sites": ["https://example.com"],
            "user_agent": ["hide_Ladybird"]
        }
    )"sv))));

    EXPECT(rule.matches(site("https://example.com"sv)));
    EXPECT(rule.matches(site("https://www.example.com"sv)));
    EXPECT(rule.matches(site("https://edition.news.example.com:8443"sv)));
    EXPECT(!rule.matches(site("http://example.com"sv)));
    EXPECT(!rule.matches(site("https://example.com.evil.test"sv)));
    EXPECT(!rule.matches(site("https://notexample.com"sv)));
    EXPECT(!rule.matches(URL::Site::obtain(URL::Origin::create_opaque())));
}

// The agents of a process share these rules, each matching on its own thread.
TEST_CASE(rule_matches_on_several_threads_at_once)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto rule = MUST(Web::SiteCompatibilityRule::create({ "https://example.com"_string }, {}));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto matching_site = site("https://www.example.com"sv);
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> wrong_answers { 0 };

    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < 4; ++i) {
        threads.append(Threading::Thread::construct("Matcher"sv, [&] -> intptr_t {
            for (size_t round = 0; round < 20'000; ++round) {
                if (!rule.matches(matching_site))
                    wrong_answers.fetch_add(1);
            }
            return 0;
        }));
    }
    for (auto& thread : threads)
        thread->start();
    for (auto& thread : threads)
        (void)thread->join();

    EXPECT_EQ(wrong_answers.load(), 0u);
}

TEST_CASE(rule_transforms_user_agent)
{
    auto rule = MUST(Web::SiteCompatibilityRule::create(
        { "https://example.com"_string },
        { Web::UserAgentTransformation::HideLadybird }));

    EXPECT_EQ(rule.apply_user_agent_transformations(default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
}

TEST_CASE(data_transforms_for_a_matching_top_level_site)
{
    Web::SiteCompatibilityData data;
    data.add_rule(MUST(Web::SiteCompatibilityRule::create(
        { "https://example.com"_string },
        { Web::UserAgentTransformation::HideLadybird })));

    EXPECT_EQ(data.user_agent_for(origin("https://www.example.com"sv), origin("https://www.example.com"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for(origin("https://www.example.com"sv), origin("https://elsewhere.example"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for(origin("https://www.example.com"sv), URL::Origin::create_opaque(), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for(origin("https://elsewhere.example"sv), origin("https://elsewhere.example"sv), default_user_agent), default_user_agent);
}

TEST_CASE(data_transforms_for_a_matching_site_under_another_top_level_site)
{
    Web::SiteCompatibilityData data;
    data.add_rule(MUST(Web::SiteCompatibilityRule::create(
        { "https://example.com"_string },
        { Web::UserAgentTransformation::HideLadybird })));

    EXPECT_EQ(data.user_agent_for(origin("https://elsewhere.example"sv), origin("https://player.example.com"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
}

TEST_CASE(data_matches_each_rule_on_its_own)
{
    Web::SiteCompatibilityData data;
    data.add_rule(MUST(Web::SiteCompatibilityRule::create(
        { "https://example.com"_string },
        { Web::UserAgentTransformation::HideLadybird })));
    data.add_rule(MUST(Web::SiteCompatibilityRule::create(
        { "https://elsewhere.example"_string },
        {},
        { "Example"_string })));

    EXPECT(data.exposes_experimental_interface(origin("https://example.com"sv), origin("https://elsewhere.example"sv), "Example"sv));
    EXPECT(!data.exposes_experimental_interface(origin("https://example.com"sv), origin("https://example.com"sv), "Example"sv));
    EXPECT_EQ(data.user_agent_for(origin("https://example.com"sv), origin("https://www.example.com"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
}

TEST_CASE(hide_ladybird_only_removes_ladybird_products)
{
    auto rule = MUST(Web::SiteCompatibilityRule::create(
        { "https://example.com"_string },
        { Web::UserAgentTransformation::HideLadybird }));

    EXPECT_EQ(rule.apply_user_agent_transformations("Ladybird/1.0"sv), String {});
    EXPECT_EQ(rule.apply_user_agent_transformations("Ladybird/1.0 Mozilla/5.0"sv), "Mozilla/5.0"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 Ladybird/1.0"sv), "Mozilla/5.0"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 Ladybird Preview/1.0"sv), "Mozilla/5.0 Ladybird Preview/1.0"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 MyLadybird/1.0"sv), "Mozilla/5.0 MyLadybird/1.0"_string);
}

TEST_CASE(hide_chromium_only_removes_chrome_products)
{
    auto rule = MUST(Web::SiteCompatibilityRule::create(
        { "https://example.com"_string },
        { Web::UserAgentTransformation::HideLadybird, Web::UserAgentTransformation::HideChromium }));

    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Ladybird/1.0 AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"sv),
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Safari/537.36"_string);
    EXPECT_EQ(rule.apply_user_agent_transformations("Mozilla/5.0 Chrome Mobile/1.0 MyChrome/1.0"sv), "Mozilla/5.0 Chrome Mobile/1.0 MyChrome/1.0"_string);
}

TEST_CASE(data_parses_rule_array)
{
    auto data = MUST(Web::SiteCompatibilityData::from_json(MUST(JsonValue::from_string(R"(
        [{
            "sites": ["https://example.com"],
            "user_agent": ["hide_Ladybird"]
        }, {
            "sites": ["https://example.org"],
            "user_agent": ["hide_Ladybird", "hide_Chromium"]
        }]
    )"sv))));

    EXPECT_EQ(data.rules().size(), 2u);
    EXPECT_EQ(data.user_agent_for(origin("https://example.org"sv), origin("https://example.org"sv), default_user_agent), "Mozilla/5.0 Safari/537.36"_string);
    EXPECT_EQ(data.user_agent_for(origin("https://www.example.com"sv), origin("https://www.example.com"sv), default_user_agent), "Mozilla/5.0 Chrome/146.0.0.0 Safari/537.36"_string);
}

TEST_CASE(invalid_rules_are_rejected)
{
    auto missing_sites = Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        { "user_agent": ["hide_Ladybird"] }
    )"sv)));
    EXPECT(missing_sites.is_error());

    auto unknown_transformation = Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        { "sites": ["https://example.com"], "user_agent": ["dance"] }
    )"sv)));
    EXPECT(unknown_transformation.is_error());

    for (auto invalid_site : { "["sv, "example.com"sv, "https://www.example.com"sv, "https://example.com/"sv,
             "https://example.com/*"sv, "https://example.com:8443"sv, "data:text/plain,"sv }) {
        auto rule = Web::SiteCompatibilityRule::create({ MUST(String::from_utf8(invalid_site)) }, { Web::UserAgentTransformation::HideLadybird });
        EXPECT(rule.is_error());
    }

    auto old_format = Web::SiteCompatibilityRule::from_json(MUST(JsonValue::from_string(R"(
        { "matches": ["https://example.com/*"], "user_agent": ["hide_Ladybird"] }
    )"sv)));
    EXPECT(old_format.is_error());
}
