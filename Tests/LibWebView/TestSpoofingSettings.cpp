/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Settings.h>

static ByteString settings_path()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-spoofing-settings-{}.json", Core::System::getpid())).string();
}

static void remove_settings_file()
{
    if (FileSystem::exists(settings_path()))
        MUST(FileSystem::remove(settings_path(), FileSystem::RecursionMode::Disallowed));
}

static void write_settings_file(StringView contents)
{
    auto file = MUST(Core::File::open(settings_path(), Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));
}

TEST_CASE(spoofing_settings_default_to_no_spoofing)
{
    remove_settings_file();

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.user_agent_preset().has_value());
    EXPECT_EQ(settings.navigator_compatibility_mode(), Web::NavigatorCompatibilityMode::Chrome);
}

TEST_CASE(spoofing_settings_are_persistent)
{
    remove_settings_file();

    {
        auto settings = WebView::Settings::create(settings_path());
        settings.set_user_agent_preset("Firefox macOS Desktop"sv);
        settings.set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode::Gecko);
    }

    {
        auto settings = WebView::Settings::create(settings_path());
        EXPECT_EQ(settings.user_agent_preset(), "Firefox macOS Desktop"sv);
        EXPECT_EQ(settings.navigator_compatibility_mode(), Web::NavigatorCompatibilityMode::Gecko);

        settings.set_user_agent_preset({});
        settings.set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode::WebKit);
    }

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.user_agent_preset().has_value());
    EXPECT_EQ(settings.navigator_compatibility_mode(), Web::NavigatorCompatibilityMode::WebKit);
    remove_settings_file();
}

TEST_CASE(saved_user_agent_preset_names_are_normalized)
{
    write_settings_file(R"({ "userAgentPreset": "safari ios mobile" })"sv);

    auto settings = WebView::Settings::create(settings_path());
    EXPECT_EQ(settings.user_agent_preset(), "Safari iOS Mobile"sv);
    remove_settings_file();
}

TEST_CASE(unknown_saved_spoofing_values_fall_back_to_defaults)
{
    write_settings_file(R"({ "userAgentPreset": "Netscape Navigator", "navigatorCompatibilityMode": "trident" })"sv);

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.user_agent_preset().has_value());
    EXPECT_EQ(settings.navigator_compatibility_mode(), Web::NavigatorCompatibilityMode::Chrome);
    remove_settings_file();
}
