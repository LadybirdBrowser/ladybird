/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Loader/LoaderConfig.h>

TEST_CASE(a_change_leaves_a_snapshot_already_taken_as_it_was)
{
    auto before = Web::current_loader_config();
    Web::update_loader_config([](auto& config) { config.preferred_languages = { "fr-CA"_string, "fr"_string }; });
    auto after = Web::current_loader_config();

    EXPECT_EQ(before->config.preferred_languages, (Vector { "en-US"_string, "en"_string }));
    EXPECT_EQ(after->config.preferred_languages, (Vector { "fr-CA"_string, "fr"_string }));
}

TEST_CASE(a_change_keeps_the_rest_of_the_configuration)
{
    auto before = Web::current_loader_config();
    Web::update_loader_config([](auto& config) { config.enable_global_privacy_control = !config.enable_global_privacy_control; });
    auto after = Web::current_loader_config();

    EXPECT_NE(after->config.enable_global_privacy_control, before->config.enable_global_privacy_control);
    EXPECT_EQ(after->config.user_agent, before->config.user_agent);
    EXPECT_EQ(after->config.preferred_languages, before->config.preferred_languages);
    EXPECT_EQ(after->config.site_compatibility.ptr(), before->config.site_compatibility.ptr());
}
