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
#include <LibURL/Parser.h>
#include <LibWebView/Settings.h>

static ByteString settings_path()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-background-networking-{}.json", Core::System::getpid())).string();
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

TEST_CASE(content_blocker_list_registry)
{
    remove_settings_file();
    auto settings = WebView::Settings::create(settings_path());
    auto const& definitions = settings.content_blocker_lists();
    EXPECT_EQ(definitions.size(), 27u);

    for (size_t i = 0; i < definitions.size(); ++i) {
        auto const& definition = definitions[i];
        EXPECT(!definition.enabled);
        EXPECT(!definition.identifier.is_empty());
        EXPECT(!definition.name.is_empty());
        EXPECT(!definition.description.is_empty());
        auto const& url = definition.url;
        EXPECT(url.has_value());
        EXPECT(url->scheme() == "https"sv);

        for (size_t j = i + 1; j < definitions.size(); ++j)
            EXPECT(definition.identifier != definitions[j].identifier);
    }
}

TEST_CASE(feature_policy)
{
    remove_settings_file();
    auto settings = WebView::Settings::create(settings_path());

    EXPECT(!settings.automatic_filter_list_updates_allowed());

    settings.set_filter_list_updates_enabled(true);
    EXPECT(settings.automatic_filter_list_updates_allowed());

    settings.set_background_networking_enabled(false);
    EXPECT(!settings.automatic_filter_list_updates_allowed());
    EXPECT(settings.filter_list_updates_enabled());

    settings.set_background_networking_enabled(true);
    EXPECT(settings.automatic_filter_list_updates_allowed());

    settings.set_filter_list_updates_enabled(false);
    EXPECT(!settings.automatic_filter_list_updates_allowed());
    remove_settings_file();
}

TEST_CASE(settings_are_persistent)
{
    remove_settings_file();

    {
        auto settings = WebView::Settings::create(settings_path());
        settings.set_filter_list_updates_enabled(false);
        settings.set_background_networking_enabled(false);
    }

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.filter_list_updates_enabled());
    EXPECT(!settings.background_networking_enabled());
    remove_settings_file();
}

TEST_CASE(content_blocker_list_settings_are_persistent)
{
    remove_settings_file();

    String subscription_identifier;
    String local_identifier;
    {
        auto settings = WebView::Settings::create(settings_path());
        EXPECT(!settings.content_blocker_list("easyList"sv)->enabled);
        EXPECT(!settings.content_blocker_list("easyPrivacy"sv)->enabled);

        settings.set_content_blocker_list_enabled("easyList"sv, false);
        settings.set_content_blocker_list_enabled("easyPrivacy"sv, true);
        subscription_identifier = settings.add_content_blocker_list("https://example.com/filters.txt"_string, URL::Parser::basic_parse("https://example.com/filters.txt"sv));
        settings.set_content_blocker_list_enabled(subscription_identifier, false);
        local_identifier = settings.add_content_blocker_list("my-filters.txt"_string);
        settings.set_content_blocker_list_enabled(local_identifier, false);
        settings.set_custom_content_blocker_filters("example.com##.advertisement"_string);
    }

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.content_blocker_list("easyList"sv)->enabled);
    EXPECT(settings.content_blocker_list("easyPrivacy"sv)->enabled);
    EXPECT_EQ(settings.content_blocker_lists().size(), 27u + 2);
    auto const& subscription = settings.content_blocker_list(subscription_identifier).value();
    EXPECT_EQ(subscription.url->serialize(), "https://example.com/filters.txt"sv);
    EXPECT(!subscription.enabled);
    auto const& local = settings.content_blocker_list(local_identifier).value();
    EXPECT_EQ(local.name, "my-filters.txt"sv);
    EXPECT(!local.url.has_value());
    EXPECT(!local.enabled);
    EXPECT_EQ(settings.custom_content_blocker_filters(), "example.com##.advertisement"sv);

    EXPECT(!settings.remove_content_blocker_list("easyList"sv));
    EXPECT(!settings.remove_content_blocker_list("unknown"sv));
    EXPECT(settings.remove_content_blocker_list(subscription_identifier));
    EXPECT(settings.remove_content_blocker_list(local_identifier));
    EXPECT_EQ(settings.content_blocker_lists().size(), 27u);

    remove_settings_file();
}

TEST_CASE(content_blocker_list_settings_reject_built_in_identifiers)
{
    remove_settings_file();
    write_settings_file(R"({
        "contentBlockers": {
            "customSubscriptions": [
                { "identifier": "easyList", "url": "https://example.com/reserved.txt", "enabled": true },
                { "identifier": "custom-list", "url": "https://example.com/custom.txt", "enabled": true }
            ],
            "localLists": [
                { "identifier": "easyPrivacy", "name": "reserved.txt", "enabled": true },
                { "identifier": "local-list", "name": "local.txt", "enabled": true }
            ]
        }
    })"sv);

    auto settings = WebView::Settings::create(settings_path());
    EXPECT_EQ(settings.content_blocker_lists().size(), 27u + 2);
    EXPECT(settings.content_blocker_list("custom-list"sv).has_value());
    EXPECT(settings.content_blocker_list("local-list"sv).has_value());
    EXPECT(settings.content_blocker_list("easyList"sv)->built_in);
    EXPECT(settings.content_blocker_list("easyPrivacy"sv)->built_in);
    remove_settings_file();
}
