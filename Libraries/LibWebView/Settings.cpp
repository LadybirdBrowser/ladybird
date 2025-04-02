/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/JsonArray.h>
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

static constexpr auto autocomplete_engine_key = "autocompleteEngine"sv;
static constexpr auto autocomplete_engine_name_key = "name"sv;

static constexpr auto site_setting_enabled_globally_key = "enabledGlobally"sv;
static constexpr auto site_setting_site_filters_key = "siteFilters"sv;

static constexpr auto autoplay_key = "autoplay"sv;

static constexpr auto do_not_track_key = "doNotTrack"sv;

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

    if (auto autocomplete_engine = settings_json.value().get_object(autocomplete_engine_key); autocomplete_engine.has_value()) {
        if (auto autocomplete_engine_name = autocomplete_engine->get_string(autocomplete_engine_name_key); autocomplete_engine_name.has_value())
            settings.m_autocomplete_engine = find_autocomplete_engine_by_name(*autocomplete_engine_name);
    }

    auto load_site_setting = [&](SiteSetting& site_setting, StringView key) {
        auto saved_settings = settings_json.value().get_object(key);
        if (!saved_settings.has_value())
            return;

        if (auto enabled_globally = saved_settings->get_bool(site_setting_enabled_globally_key); enabled_globally.has_value())
            site_setting.enabled_globally = *enabled_globally;

        if (auto site_filters = saved_settings->get_array(site_setting_site_filters_key); site_filters.has_value()) {
            site_setting.site_filters.clear();

            site_filters->for_each([&](auto const& site_filter) {
                if (site_filter.is_string())
                    site_setting.site_filters.set(site_filter.as_string());
            });
        }
    };

    load_site_setting(settings.m_autoplay, autoplay_key);

    if (auto do_not_track = settings_json.value().get_bool(do_not_track_key); do_not_track.has_value())
        settings.m_do_not_track = *do_not_track ? DoNotTrack::Yes : DoNotTrack::No;

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

    if (m_autocomplete_engine.has_value()) {
        JsonObject autocomplete_engine;
        autocomplete_engine.set(autocomplete_engine_name_key, m_autocomplete_engine->name);

        settings.set(autocomplete_engine_key, move(autocomplete_engine));
    }

    auto save_site_setting = [&](SiteSetting const& site_setting, StringView key) {
        JsonArray site_filters;
        site_filters.ensure_capacity(site_setting.site_filters.size());

        for (auto const& site_filter : site_setting.site_filters)
            site_filters.must_append(site_filter);

        JsonObject setting;
        setting.set("enabledGlobally"sv, site_setting.enabled_globally);
        setting.set("siteFilters"sv, move(site_filters));

        settings.set(key, move(setting));
    };

    save_site_setting(m_autoplay, autoplay_key);

    settings.set(do_not_track_key, m_do_not_track == DoNotTrack::Yes);

    return settings;
}

void Settings::restore_defaults()
{
    m_new_tab_page_url = URL::about_newtab();
    m_search_engine.clear();
    m_autocomplete_engine.clear();
    m_autoplay = SiteSetting {};
    m_do_not_track = DoNotTrack::No;

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

void Settings::set_autocomplete_engine(Optional<StringView> autocomplete_engine_name)
{
    if (autocomplete_engine_name.has_value())
        m_autocomplete_engine = find_autocomplete_engine_by_name(*autocomplete_engine_name);
    else
        m_autocomplete_engine.clear();

    persist_settings();

    for (auto& observer : m_observers)
        observer.autocomplete_engine_changed();
}

void Settings::set_autoplay_enabled_globally(bool enabled_globally)
{
    m_autoplay.enabled_globally = enabled_globally;
    persist_settings();

    for (auto& observer : m_observers)
        observer.autoplay_settings_changed();
}

void Settings::add_autoplay_site_filter(String const& site_filter)
{
    auto trimmed_site_filter = MUST(site_filter.trim_whitespace());
    if (trimmed_site_filter.is_empty())
        return;

    m_autoplay.site_filters.set(move(trimmed_site_filter));
    persist_settings();

    for (auto& observer : m_observers)
        observer.autoplay_settings_changed();
}

void Settings::remove_autoplay_site_filter(String const& site_filter)
{
    m_autoplay.site_filters.remove(site_filter);
    persist_settings();

    for (auto& observer : m_observers)
        observer.autoplay_settings_changed();
}

void Settings::remove_all_autoplay_site_filters()
{
    m_autoplay.site_filters.clear();
    persist_settings();

    for (auto& observer : m_observers)
        observer.autoplay_settings_changed();
}

void Settings::set_do_not_track(DoNotTrack do_not_track)
{
    m_do_not_track = do_not_track;
    persist_settings();

    for (auto& observer : m_observers)
        observer.do_not_track_changed();
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

SiteSetting::SiteSetting()
{
    site_filters.set("file://"_string);
}

}
