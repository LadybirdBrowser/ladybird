/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Impersonation.h>

using namespace RequestServer;

TEST_CASE(target_follows_the_browser_a_user_agent_claims)
{
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.2 Safari/605.1.15"sv), "safari2601"sv);
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (iPhone; CPU iPhone OS 27_2 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.2 Mobile/15E148 Safari/604.1"sv), "safari260_ios"sv);
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"sv), "chrome150"sv);
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Mobile Safari/537.36"sv), "chrome131_android"sv);
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:156.0) Gecko/20100101 Firefox/156.0"sv), "firefox147"sv);
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (Android 13; Mobile; rv:156.0) Gecko/156.0 Firefox/156.0"sv), "firefox147"sv);

    // Ladybird's own User-Agent names Chrome, so it gets Chrome's handshake.
    EXPECT_EQ(impersonation_target_for_user_agent("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Ladybird/1.0 Chrome/153.0.0.0 AppleWebKit/537.36 Safari/537.36"sv), "chrome150"sv);

    // A string that claims no browser gets libcurl's own handshake.
    EXPECT(!impersonation_target_for_user_agent("curl/8.22.0"sv).has_value());
    EXPECT(!impersonation_target_for_user_agent(""sv).has_value());
    EXPECT(!impersonation_target_for_user_agent("Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)"sv).has_value());
}

TEST_CASE(every_target_the_mapping_names_exists_in_libcurl)
{
    // Every profile the mapping can answer with has to be one libcurl knows, or every request on it would fail.
    static constexpr Array user_agents {
        "Version/27.2 Safari/605.1.15"sv,
        "iPhone Version/27.2 Mobile/15E148 Safari/604.1"sv,
        "Chrome/153.0.0.0 Safari/537.36"sv,
        "Android Chrome/153.0.0.0 Mobile Safari/537.36"sv,
        "Firefox/156.0"sv,
    };
    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (auto user_agent : user_agents) {
        auto* handle = curl_easy_init();
        VERIFY(handle);
        auto applied = impersonate_user_agent(handle, user_agent);
#if defined(REQUESTSERVER_CURL_IMPERSONATE)
        EXPECT(applied);
#else
        EXPECT(!applied);
#endif
        curl_easy_cleanup(handle);
    }
    curl_global_cleanup();
}
