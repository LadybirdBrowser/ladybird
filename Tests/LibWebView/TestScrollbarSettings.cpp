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

TEST_CASE(always_show_scrollbars_is_off_by_default_and_persists)
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-scrollbar-settings-{}.json", Core::System::getpid())).string();
    if (FileSystem::exists(path))
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    {
        auto settings = WebView::Settings::create(path);
        EXPECT(!settings.appearance().always_show_scrollbars);
        struct Observer : WebView::SettingsObserver {
            explicit Observer(WebView::Settings& settings)
                : SettingsObserver(settings)
            {
            }
            virtual void appearance_changed() override { ++changes; }
            size_t changes { 0 };
        } observer(settings);
        auto appearance = settings.appearance();
        appearance.always_show_scrollbars = true;
        settings.set_appearance(appearance);
        EXPECT_EQ(observer.changes, 1u);
    }
    {
        auto settings = WebView::Settings::create(path);
        EXPECT(settings.appearance().always_show_scrollbars);
        auto appearance = settings.appearance();
        appearance.always_show_scrollbars = false;
        settings.set_appearance(appearance);
    }
    auto settings = WebView::Settings::create(path);
    EXPECT(!settings.appearance().always_show_scrollbars);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}

TEST_CASE(always_show_scrollbars_ignores_non_boolean_values)
{
    auto json = MUST(JsonValue::from_string(R"({"alwaysShowScrollbars":"true"})"sv));
    EXPECT(!WebView::Settings::parse_appearance(json).always_show_scrollbars);
}
