/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Settings.h>

TEST_CASE(force_dark_choices_survive_default_changes_and_restart)
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-force-dark-{}.json", Core::System::getpid())).string();
    if (FileSystem::exists(path))
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    {
        auto settings = WebView::Settings::create(path);
        EXPECT(!settings.force_dark_for_host("example.com"sv));
        settings.set_force_dark_for_host("example.com"_string, false);
        settings.set_force_dark_for_host("other.example"_string, true);
        settings.set_force_dark_for_host(String {}, true);
        EXPECT_EQ(settings.content_settings().force_dark_per_host.size(), 2u);
        auto content = settings.content_settings();
        content.enable_force_dark = true;
        settings.set_content_settings(move(content));
        EXPECT(!settings.force_dark_for_host("example.com"sv));
        EXPECT(settings.force_dark_for_host("other.example"sv));
        EXPECT(settings.force_dark_for_host("sub.example.com"sv));
        EXPECT(settings.force_dark_for_host({}));
    }
    {
        auto settings = WebView::Settings::create(path);
        EXPECT(!settings.force_dark_for_host("example.com"sv));
        EXPECT(settings.force_dark_for_host("other.example"sv));
        settings.reset_force_dark_for_host("example.com"sv);
        EXPECT(settings.force_dark_for_host("example.com"sv));
        EXPECT_EQ(settings.content_settings().force_dark_per_host.size(), 1u);
    }
    auto settings = WebView::Settings::create(path);
    EXPECT(settings.force_dark_for_host("example.com"sv));
    EXPECT_EQ(settings.content_settings().force_dark_per_host.size(), 1u);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}

TEST_CASE(force_dark_ignores_invalid_saved_choices)
{
    auto json = MUST(JsonValue::from_string(R"({"enableForceDark":true,"forceDarkPerHost":{"on.example":true,"off.example":false,"string.example":"false","number.example":1,"null.example":null,"":false}})"sv));
    auto content = WebView::Settings::parse_content_settings(json);
    EXPECT(content.enable_force_dark);
    EXPECT_EQ(content.force_dark_per_host.size(), 2u);
    EXPECT_EQ(content.force_dark_per_host.get("on.example"sv), true);
    EXPECT_EQ(content.force_dark_per_host.get("off.example"sv), false);
}

TEST_CASE(force_dark_notifies_only_when_a_choice_changes)
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-force-dark-observer-{}.json", Core::System::getpid())).string();
    if (FileSystem::exists(path))
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    auto settings = WebView::Settings::create(path);
    struct Observer : WebView::SettingsObserver {
        explicit Observer(WebView::Settings& settings)
            : SettingsObserver(settings)
        {
        }
        virtual void content_settings_changed() override { ++changes; }
        size_t changes { 0 };
    } observer(settings);
    settings.set_force_dark_for_host("example.com"_string, false);
    EXPECT_EQ(observer.changes, 1u);
    settings.set_force_dark_for_host("example.com"_string, false);
    settings.set_force_dark_for_host({}, true);
    settings.reset_force_dark_for_host("missing.example"sv);
    EXPECT_EQ(observer.changes, 1u);
    settings.set_force_dark_for_host("example.com"_string, true);
    settings.reset_force_dark_for_host("example.com"sv);
    EXPECT_EQ(observer.changes, 3u);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}
