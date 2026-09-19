/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Process.h>

TEST_CASE(helper_environment_keeps_only_allowed_variables)
{
    MUST(Core::Environment::set("GITHUB_TOKEN"sv, "secret"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LADYBIRD_TEST_SWITCH"sv, "1"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LIBWEB_TEST_SWITCH"sv, "1"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("LC_ALL"sv, "en_US.UTF-8"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("HOME"sv, "/Users/test"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("https_proxy"sv, "http://proxy:3128"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("NO_PROXY"sv, "localhost"sv, Core::Environment::Overwrite::Yes));

    auto environment = WebView::Process::helper_process_environment(WebView::ProcessType::WebContent);
    EXPECT(environment.contains_slow("LADYBIRD_TEST_SWITCH=1"sv));
    EXPECT(environment.contains_slow("LIBWEB_TEST_SWITCH=1"sv));
    EXPECT(environment.contains_slow("LC_ALL=en_US.UTF-8"sv));
    EXPECT(environment.contains_slow("HOME=/Users/test"sv));
    EXPECT(!environment.contains_slow("GITHUB_TOKEN=secret"sv));

    // Only RequestServer talks to the network, so only it learns about proxies.
    EXPECT(!environment.contains_slow("https_proxy=http://proxy:3128"sv));
    auto request_server_environment = WebView::Process::helper_process_environment(WebView::ProcessType::RequestServer);
    EXPECT(request_server_environment.contains_slow("https_proxy=http://proxy:3128"sv));
    EXPECT(request_server_environment.contains_slow("NO_PROXY=localhost"sv));
    EXPECT(request_server_environment.contains_slow("LADYBIRD_TEST_SWITCH=1"sv));
    EXPECT(!request_server_environment.contains_slow("GITHUB_TOKEN=secret"sv));
}
