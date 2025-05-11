/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Find.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibURL/Parser.h>
#include <LibUnicode/Locale.h>
#include <LibWebView/Application.h>
#include <LibWebView/Settings.h>

namespace WebView {

static constexpr auto new_tab_page_url_key = "newTabPageURL"sv;

static constexpr auto languages_key = "languages"sv;
static auto default_language = "en"_string;

static constexpr auto search_engine_key = "searchEngine"sv;
static constexpr auto search_engine_custom_key = "custom"sv;
static constexpr auto search_engine_name_key = "name"sv;
static constexpr auto search_engine_url_key = "url"sv;

static constexpr auto autocomplete_engine_key = "autocompleteEngine"sv;
static constexpr auto autocomplete_engine_name_key = "name"sv;

static constexpr auto site_setting_enabled_globally_key = "enabledGlobally"sv;
static constexpr auto site_setting_site_filters_key = "siteFilters"sv;

static constexpr auto autoplay_key = "autoplay"sv;

static constexpr auto do_not_track_key = "doNotTrack"sv;

static constexpr auto dns_settings_key = "dnsSettings"sv;

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

    if (auto languages = settings_json.value().get(languages_key); languages.has_value())
        settings.m_languages = parse_json_languages(*languages);

    if (auto search_engine = settings_json.value().get_object(search_engine_key); search_engine.has_value()) {
        if (auto custom_engines = search_engine->get_array(search_engine_custom_key); custom_engines.has_value()) {
            custom_engines->for_each([&](JsonValue const& engine) {
                auto custom_engine = parse_custom_search_engine(engine);
                if (!custom_engine.has_value() || settings.find_search_engine_by_name(custom_engine->name).has_value())
                    return;

                settings.m_custom_search_engines.append(custom_engine.release_value());
            });
        }

        if (auto search_engine_name = search_engine->get_string(search_engine_name_key); search_engine_name.has_value())
            settings.m_search_engine = settings.find_search_engine_by_name(*search_engine_name);
    }

    if (settings.m_search_engine.has_value()) {
        if (auto autocomplete_engine = settings_json.value().get_object(autocomplete_engine_key); autocomplete_engine.has_value()) {
            if (auto autocomplete_engine_name = autocomplete_engine->get_string(autocomplete_engine_name_key); autocomplete_engine_name.has_value())
                settings.m_autocomplete_engine = find_autocomplete_engine_by_name(*autocomplete_engine_name);
        }
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

    if (auto dns_settings = settings_json.value().get(dns_settings_key); dns_settings.has_value())
        settings.m_dns_settings = parse_dns_settings(*dns_settings);

    return settings;
}

Settings::Settings(ByteString settings_path)
    : m_settings_path(move(settings_path))
    , m_new_tab_page_url(URL::about_newtab())
    , m_languages({ default_language })
{
}

JsonValue Settings::serialize_json() const
{
    JsonObject settings;
    settings.set(new_tab_page_url_key, m_new_tab_page_url.serialize());

    JsonArray languages;
    languages.ensure_capacity(m_languages.size());

    for (auto const& language : m_languages)
        languages.must_append(language);

    settings.set(languages_key, move(languages));

    JsonArray custom_search_engines;
    custom_search_engines.ensure_capacity(m_custom_search_engines.size());

    for (auto const& engine : m_custom_search_engines) {
        JsonObject search_engine;
        search_engine.set(search_engine_name_key, engine.name);
        search_engine.set(search_engine_url_key, engine.query_url);

        custom_search_engines.must_append(move(search_engine));
    }

    JsonObject search_engine;
    if (!custom_search_engines.is_empty())
        search_engine.set(search_engine_custom_key, move(custom_search_engines));
    if (m_search_engine.has_value())
        search_engine.set(search_engine_name_key, m_search_engine->name);

    if (!search_engine.is_empty())
        settings.set(search_engine_key, move(search_engine));

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

    // dnsSettings :: { mode: "system" } | { mode: "custom", server: string, port: u16, type: "udp" | "tls", forciblyEnabled: bool }
    JsonObject dns_settings;
    m_dns_settings.visit(
        [&](SystemDNS) {
            dns_settings.set("mode"sv, "system"sv);
        },
        [&](DNSOverTLS const& dot) {
            dns_settings.set("mode"sv, "custom"sv);
            dns_settings.set("server"sv, dot.server_address.view());
            dns_settings.set("port"sv, dot.port);
            dns_settings.set("type"sv, "tls"sv);
            dns_settings.set("forciblyEnabled"sv, m_dns_override_by_command_line);
        },
        [&](DNSOverUDP const& dns) {
            dns_settings.set("mode"sv, "custom"sv);
            dns_settings.set("server"sv, dns.server_address.view());
            dns_settings.set("port"sv, dns.port);
            dns_settings.set("type"sv, "udp"sv);
            dns_settings.set("forciblyEnabled"sv, m_dns_override_by_command_line);
        });
    settings.set(dns_settings_key, move(dns_settings));

    return settings;
}

void Settings::restore_defaults()
{
    m_new_tab_page_url = URL::about_newtab();
    m_languages = { default_language };
    m_search_engine.clear();
    m_custom_search_engines.clear();
    m_autocomplete_engine.clear();
    m_autoplay = SiteSetting {};
    m_do_not_track = DoNotTrack::No;
    m_dns_settings = SystemDNS {};

    persist_settings();

    for (auto& observer : m_observers) {
        observer.new_tab_page_url_changed();
        observer.languages_changed();
        observer.search_engine_changed();
        observer.autocomplete_engine_changed();
        observer.autoplay_settings_changed();
        observer.do_not_track_changed();
        observer.dns_settings_changed();
    }
}

void Settings::set_new_tab_page_url(URL::URL new_tab_page_url)
{
    m_new_tab_page_url = move(new_tab_page_url);
    persist_settings();

    for (auto& observer : m_observers)
        observer.new_tab_page_url_changed();
}

Vector<String> Settings::parse_json_languages(JsonValue const& languages)
{
    if (!languages.is_array())
        return { default_language };

    Vector<String> parsed_languages;
    parsed_languages.ensure_capacity(languages.as_array().size());

    languages.as_array().for_each([&](JsonValue const& language) {
        if (language.is_string() && Unicode::is_locale_available(language.as_string()))
            parsed_languages.append(language.as_string());
    });

    if (parsed_languages.is_empty())
        return { default_language };

    return parsed_languages;
}

void Settings::set_languages(Vector<String> languages)
{
    m_languages = move(languages);
    persist_settings();

    for (auto& observer : m_observers)
        observer.languages_changed();
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

Optional<SearchEngine> Settings::parse_custom_search_engine(JsonValue const& search_engine)
{
    if (!search_engine.is_object())
        return {};

    auto name = search_engine.as_object().get_string(search_engine_name_key);
    auto url = search_engine.as_object().get_string(search_engine_url_key);
    if (!name.has_value() || !url.has_value())
        return {};

    auto parsed_url = URL::Parser::basic_parse(*url);
    if (!parsed_url.has_value())
        return {};

    return SearchEngine { .name = name.release_value(), .query_url = url.release_value() };
}

void Settings::add_custom_search_engine(SearchEngine search_engine)
{
    if (find_search_engine_by_name(search_engine.name).has_value())
        return;

    m_custom_search_engines.append(move(search_engine));
    persist_settings();
}

void Settings::remove_custom_search_engine(SearchEngine const& search_engine)
{
    auto reset_default_search_engine = m_search_engine.has_value() && m_search_engine->name == search_engine.name;
    if (reset_default_search_engine)
        m_search_engine.clear();

    m_custom_search_engines.remove_all_matching([&](auto const& engine) {
        return engine.name == search_engine.name;
    });

    persist_settings();

    if (reset_default_search_engine) {
        for (auto& observer : m_observers)
            observer.search_engine_changed();
    }
}

Optional<SearchEngine> Settings::find_search_engine_by_name(StringView name)
{
    auto comparator = [&](auto const& engine) { return engine.name == name; };

    if (auto result = find_value(builtin_search_engines(), comparator); result.has_value())
        return result.copy();
    if (auto result = find_value(m_custom_search_engines, comparator); result.has_value())
        return result.copy();

    return {};
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

DNSSettings Settings::parse_dns_settings(JsonValue const& dns_settings)
{
    if (!dns_settings.is_object())
        return SystemDNS {};

    auto const& dns_settings_object = dns_settings.as_object();

    if (auto mode = dns_settings_object.get_string("mode"sv); mode.has_value()) {
        if (*mode == "system"sv)
            return SystemDNS {};

        if (*mode == "custom"sv) {
            auto server = dns_settings_object.get_string("server"sv);
            auto port = dns_settings_object.get_u16("port"sv);
            auto type = dns_settings_object.get_string("type"sv);

            if (server.has_value() && port.has_value() && type.has_value()) {
                if (*type == "tls"sv)
                    return DNSOverTLS { .server_address = server->to_byte_string(), .port = *port };
                if (*type == "udp"sv)
                    return DNSOverUDP { .server_address = server->to_byte_string(), .port = *port };
            }
        }
    }

    dbgln("Invalid DNS settings in parse_dns_settings, falling back to system DNS");
    return SystemDNS {};
}

void Settings::set_dns_settings(DNSSettings const& dns_settings, bool override_by_command_line)
{
    m_dns_settings = dns_settings;
    m_dns_override_by_command_line = override_by_command_line;

    if (!override_by_command_line)
        persist_settings();

    for (auto& observer : m_observers)
        observer.dns_settings_changed();
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
