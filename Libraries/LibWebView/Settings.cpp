/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/Settings.h>

namespace WebView {

static constexpr auto new_tab_page_url_key = "newTabPageURL"sv;
static constexpr auto search_engine_key = "searchEngine"sv;
static constexpr auto search_engine_name_key = "name"sv;

static ErrorOr<JsonObject> read_settings_file(StringView settings_path)
{
    auto settings_file = Core::File::open(settings_path, Core::File::OpenMode::Read);
    if (settings_file.is_error()) {
        if (settings_file.error().is_errno() && settings_file.error().code() == ENOENT)
            return JsonObject {};
        return settings_file.release_error();
    }

    auto settings_contents = TRY(settings_file.value()->read_until_eof());
    auto settings_json = TRY(JsonValue::from_string(settings_contents));

    if (!settings_json.is_object())
        return Error::from_string_literal("Expected Ladybird settings to be a JSON object");
    return move(settings_json.as_object());
}

static ErrorOr<void> write_settings_file(StringView settings_path, JsonValue const& contents)
{
    auto settings_directory = LexicalPath { settings_path }.parent();
    TRY(Core::Directory::create(settings_directory, Core::Directory::CreateDirectories::Yes));

    auto settings_file = TRY(Core::File::open(settings_path, Core::File::OpenMode::Write));
    TRY(settings_file->write_until_depleted(contents.serialized()));

    return {};
}

Settings Settings::create(Badge<Application>)
{
    // FIXME: Move this to a generic "Ladybird config directory" helper.
    auto settings_directory = ByteString::formatted("{}/Ladybird", Core::StandardPaths::config_directory());
    auto settings_path = ByteString::formatted("{}/Settings.json", settings_directory);

    Settings settings { move(settings_path) };

    auto settings_json = read_settings_file(settings.m_settings_path);
    if (settings_json.is_error()) {
        warnln("Unable to read Ladybird settings: {}", settings_json.error());
        return settings;
    }

    if (auto new_tab_page_url = settings_json.value().get_string(new_tab_page_url_key); new_tab_page_url.has_value()) {
        if (auto parsed_new_tab_page_url = URL::Parser::basic_parse(*new_tab_page_url); parsed_new_tab_page_url.has_value())
            settings.m_new_tab_page_url = parsed_new_tab_page_url.release_value();
    }

    if (auto search_engine = settings_json.value().get_object(search_engine_key); search_engine.has_value()) {
        if (auto search_engine_name = search_engine->get_string(search_engine_name_key); search_engine_name.has_value())
            settings.m_search_engine = find_search_engine_by_name(*search_engine_name);
    }

    return settings;
}

Settings::Settings(ByteString settings_path)
    : m_settings_path(move(settings_path))
    , m_new_tab_page_url(URL::about_newtab())
{
}

JsonValue Settings::serialize_json() const
{
    JsonObject settings;
    settings.set(new_tab_page_url_key, m_new_tab_page_url.serialize());

    if (m_search_engine.has_value()) {
        JsonObject search_engine;
        search_engine.set(search_engine_name_key, m_search_engine->name);

        settings.set(search_engine_key, move(search_engine));
    }

    return settings;
}

void Settings::restore_defaults()
{
    m_new_tab_page_url = URL::about_newtab();
    m_search_engine.clear();

    persist_settings();

    for (auto& observer : m_observers)
        observer.new_tab_page_url_changed();
}

void Settings::set_new_tab_page_url(URL::URL new_tab_page_url)
{
    m_new_tab_page_url = move(new_tab_page_url);
    persist_settings();

    for (auto& observer : m_observers)
        observer.new_tab_page_url_changed();
}

void Settings::set_search_engine(Optional<StringView> search_engine_name)
{
    if (search_engine_name.has_value())
        m_search_engine = find_search_engine_by_name(*search_engine_name);
    else
        m_search_engine.clear();

    persist_settings();

    for (auto& observer : m_observers)
        observer.search_engine_changed();
}

void Settings::persist_settings()
{
    auto settings = serialize_json();

    if (auto result = write_settings_file(m_settings_path, settings); result.is_error())
        warnln("Unable to persist Ladybird settings: {}", result.error());
}

void Settings::add_observer(Badge<SettingsObserver>, SettingsObserver& observer)
{
    Application::settings().m_observers.append(observer);
}

void Settings::remove_observer(Badge<SettingsObserver>, SettingsObserver& observer)
{
    auto was_removed = Application::settings().m_observers.remove_first_matching([&](auto const& candidate) {
        return &candidate == &observer;
    });
    VERIFY(was_removed);
}

SettingsObserver::SettingsObserver()
{
    Settings::add_observer({}, *this);
}

SettingsObserver::~SettingsObserver()
{
    Settings::remove_observer({}, *this);
}

}
