/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Find.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/StandardPaths.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/Parser.h>
#include <LibUnicode/Locale.h>
#include <LibWebView/Application.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Utilities.h>

namespace WebView {

static constexpr auto NEW_TAB_PAGE_URL_KEY = "newTabPageURL"sv;

static constexpr auto SHOW_BOOKMARKS_BAR_KEY = "showBookmarksBar"sv;
static constexpr auto DEFAULT_SHOW_BOOKMARKS_BAR = true;

static constexpr auto DEFAULT_ZOOM_LEVEL_FACTOR_KEY = "defaultZoomLevelFactor"sv;
static constexpr double INITIAL_ZOOM_LEVEL_FACTOR = 1.0;

static constexpr auto LANGUAGES_KEY = "languages"sv;
static auto DEFAULT_LANGUAGE = "en"_string;

static constexpr auto BROWSING_BEHAVIOR_KEY = "browsingBehavior"sv;
static constexpr auto ENABLE_AUTOSCROLL_KEY = "enableAutoscroll"sv;

static constexpr auto SEARCH_ENGINE_KEY = "searchEngine"sv;
static constexpr auto SEARCH_ENGINE_CUSTOM_KEY = "custom"sv;
static constexpr auto SEARCH_ENGINE_NAME_KEY = "name"sv;
static constexpr auto SEARCH_ENGINE_URL_KEY = "url"sv;

static constexpr auto AUTOCOMPLETE_ENGINE_KEY = "autocompleteEngine"sv;
static constexpr auto AUTOCOMPLETE_ENGINE_NAME_KEY = "name"sv;

static constexpr auto SITE_SETTING_ENABLED_GLOBALLY_KEY = "enabledGlobally"sv;
static constexpr auto SITE_SETTING_SITE_FILTERS_KEY = "siteFilters"sv;

static constexpr auto AUTOPLAY_KEY = "autoplay"sv;

static constexpr auto BROWSING_DATA_KEY = "browsingData"sv;
static constexpr auto DISK_CACHE_KEY = "diskCache"sv;
static constexpr auto DISK_CACHE_MAXIMUM_SIZE_KEY = "maxSize"sv;

static constexpr auto GLOBAL_PRIVACY_CONTROL_KEY = "globalPrivacyControl"sv;

static constexpr auto DNS_SETTINGS_KEY = "dnsSettings"sv;

Settings Settings::create(Badge<Application>)
{
    // FIXME: Move this to a generic "Ladybird config directory" helper.
    auto settings_directory = ByteString::formatted("{}/Ladybird", Core::StandardPaths::config_directory());
    auto settings_path = ByteString::formatted("{}/Settings.json", settings_directory);

    Settings settings { move(settings_path) };

    auto settings_json = read_json_file(settings.m_settings_path);
    if (settings_json.is_error()) {
        warnln("Unable to read Ladybird settings: {}", settings_json.error());
        return settings;
    }

    if (auto new_tab_page_url = settings_json.value().get_string(NEW_TAB_PAGE_URL_KEY); new_tab_page_url.has_value()) {
        if (auto parsed_new_tab_page_url = URL::Parser::basic_parse(*new_tab_page_url); parsed_new_tab_page_url.has_value())
            settings.m_new_tab_page_url = parsed_new_tab_page_url.release_value();
    }

    if (auto show_bookmarks_bar = settings_json.value().get_bool(SHOW_BOOKMARKS_BAR_KEY); show_bookmarks_bar.has_value())
        settings.m_show_bookmarks_bar = *show_bookmarks_bar;

    if (auto factor = settings_json.value().get_double_with_precision_loss(DEFAULT_ZOOM_LEVEL_FACTOR_KEY); factor.has_value())
        settings.m_default_zoom_level_factor = factor.release_value();

    if (auto languages = settings_json.value().get(LANGUAGES_KEY); languages.has_value())
        settings.m_languages = parse_json_languages(*languages);

    if (auto browsing_behavior = settings_json.value().get(BROWSING_BEHAVIOR_KEY); browsing_behavior.has_value())
        settings.m_browsing_behavior = parse_browsing_behavior(*browsing_behavior);

    if (auto search_engine = settings_json.value().get_object(SEARCH_ENGINE_KEY); search_engine.has_value()) {
        if (auto custom_engines = search_engine->get_array(SEARCH_ENGINE_CUSTOM_KEY); custom_engines.has_value()) {
            custom_engines->for_each([&](JsonValue const& engine) {
                auto custom_engine = parse_custom_search_engine(engine);
                if (!custom_engine.has_value() || settings.find_search_engine_by_name(custom_engine->name).has_value())
                    return;

                settings.m_custom_search_engines.append(custom_engine.release_value());
            });
        }

        if (auto search_engine_name = search_engine->get_string(SEARCH_ENGINE_NAME_KEY); search_engine_name.has_value())
            settings.m_search_engine = settings.find_search_engine_by_name(*search_engine_name);
    }

    if (settings.m_search_engine.has_value()) {
        if (auto autocomplete_engine = settings_json.value().get_object(AUTOCOMPLETE_ENGINE_KEY); autocomplete_engine.has_value()) {
            if (auto autocomplete_engine_name = autocomplete_engine->get_string(AUTOCOMPLETE_ENGINE_NAME_KEY); autocomplete_engine_name.has_value())
                settings.m_autocomplete_engine = find_autocomplete_engine_by_name(*autocomplete_engine_name);
        }
    }

    auto load_site_setting = [&](SiteSetting& site_setting, StringView key) {
        auto saved_settings = settings_json.value().get_object(key);
        if (!saved_settings.has_value())
            return;

        if (auto enabled_globally = saved_settings->get_bool(SITE_SETTING_ENABLED_GLOBALLY_KEY); enabled_globally.has_value())
            site_setting.enabled_globally = *enabled_globally;

        if (auto site_filters = saved_settings->get_array(SITE_SETTING_SITE_FILTERS_KEY); site_filters.has_value()) {
            site_setting.site_filters.clear();

            site_filters->for_each([&](auto const& site_filter) {
                if (site_filter.is_string())
                    site_setting.site_filters.set(site_filter.as_string());
            });
        }
    };

    load_site_setting(settings.m_autoplay, AUTOPLAY_KEY);

    if (auto browsing_data_settings = settings_json.value().get(BROWSING_DATA_KEY); browsing_data_settings.has_value())
        settings.m_browsing_data_settings = parse_browsing_data_settings(*browsing_data_settings);

    if (auto global_privacy_control = settings_json.value().get_bool(GLOBAL_PRIVACY_CONTROL_KEY); global_privacy_control.has_value())
        settings.m_global_privacy_control = *global_privacy_control ? GlobalPrivacyControl::Yes : GlobalPrivacyControl::No;

    if (auto dns_settings = settings_json.value().get(DNS_SETTINGS_KEY); dns_settings.has_value())
        settings.m_dns_settings = parse_dns_settings(*dns_settings);

    return settings;
}

Settings::Settings(ByteString settings_path)
    : m_settings_path(move(settings_path))
    , m_new_tab_page_url(URL::about_newtab())
    , m_show_bookmarks_bar(DEFAULT_SHOW_BOOKMARKS_BAR)
    , m_default_zoom_level_factor(INITIAL_ZOOM_LEVEL_FACTOR)
    , m_languages({ DEFAULT_LANGUAGE })
{
}

JsonValue Settings::serialize_json() const
{
    JsonObject settings;
    settings.set(NEW_TAB_PAGE_URL_KEY, m_new_tab_page_url.serialize());
    settings.set(SHOW_BOOKMARKS_BAR_KEY, m_show_bookmarks_bar);
    settings.set(DEFAULT_ZOOM_LEVEL_FACTOR_KEY, m_default_zoom_level_factor);

    JsonArray languages;
    languages.ensure_capacity(m_languages.size());

    for (auto const& language : m_languages)
        languages.must_append(language);

    settings.set(LANGUAGES_KEY, move(languages));

    JsonObject browsing_behavior;
    browsing_behavior.set(ENABLE_AUTOSCROLL_KEY, m_browsing_behavior.enable_autoscroll);
    settings.set(BROWSING_BEHAVIOR_KEY, move(browsing_behavior));

    JsonArray custom_search_engines;
    custom_search_engines.ensure_capacity(m_custom_search_engines.size());

    for (auto const& engine : m_custom_search_engines) {
        JsonObject search_engine;
        search_engine.set(SEARCH_ENGINE_NAME_KEY, engine.name);
        search_engine.set(SEARCH_ENGINE_URL_KEY, engine.query_url);

        custom_search_engines.must_append(move(search_engine));
    }

    JsonObject search_engine;
    if (!custom_search_engines.is_empty())
        search_engine.set(SEARCH_ENGINE_CUSTOM_KEY, move(custom_search_engines));
    if (m_search_engine.has_value())
        search_engine.set(SEARCH_ENGINE_NAME_KEY, m_search_engine->name);

    if (!search_engine.is_empty())
        settings.set(SEARCH_ENGINE_KEY, move(search_engine));

    if (m_autocomplete_engine.has_value()) {
        JsonObject autocomplete_engine;
        autocomplete_engine.set(AUTOCOMPLETE_ENGINE_NAME_KEY, m_autocomplete_engine->name);

        settings.set(AUTOCOMPLETE_ENGINE_KEY, move(autocomplete_engine));
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

    save_site_setting(m_autoplay, AUTOPLAY_KEY);

    JsonObject disk_cache_settings;
    disk_cache_settings.set(DISK_CACHE_MAXIMUM_SIZE_KEY, m_browsing_data_settings.disk_cache_settings.maximum_size);

    JsonObject browsing_data;
    browsing_data.set(DISK_CACHE_KEY, move(disk_cache_settings));
    settings.set(BROWSING_DATA_KEY, move(browsing_data));

    settings.set(GLOBAL_PRIVACY_CONTROL_KEY, m_global_privacy_control == GlobalPrivacyControl::Yes);

    // dnsSettings :: { mode: "system" } | { mode: "custom", server: string, port: u16, type: "udp" | "tls", forciblyEnabled: bool, dnssec: bool }
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
            dns_settings.set("dnssec"sv, dot.validate_dnssec_locally);
            dns_settings.set("forciblyEnabled"sv, m_dns_override_by_command_line);
        },
        [&](DNSOverUDP const& dns) {
            dns_settings.set("mode"sv, "custom"sv);
            dns_settings.set("server"sv, dns.server_address.view());
            dns_settings.set("port"sv, dns.port);
            dns_settings.set("type"sv, "udp"sv);
            dns_settings.set("dnssec"sv, dns.validate_dnssec_locally);
            dns_settings.set("forciblyEnabled"sv, m_dns_override_by_command_line);
        });
    settings.set(DNS_SETTINGS_KEY, move(dns_settings));

    return settings;
}

void Settings::set_new_tab_page_url(URL::URL new_tab_page_url)
{
    m_new_tab_page_url = move(new_tab_page_url);
    persist_settings();

    for (auto& observer : m_observers)
        observer.new_tab_page_url_changed();
}

void Settings::set_show_bookmarks_bar(bool show_bookmarks_bar)
{
    m_show_bookmarks_bar = show_bookmarks_bar;
    persist_settings();

    for (auto& observer : m_observers)
        observer.show_bookmarks_bar_changed();
}

void Settings::set_default_zoom_level_factor(double zoom_level)
{
    m_default_zoom_level_factor = zoom_level;
    persist_settings();

    for (auto& observer : m_observers)
        observer.default_zoom_level_factor_changed();
}

Vector<String> Settings::parse_json_languages(JsonValue const& languages)
{
    if (!languages.is_array())
        return { DEFAULT_LANGUAGE };

    Vector<String> parsed_languages;
    parsed_languages.ensure_capacity(languages.as_array().size());

    languages.as_array().for_each([&](JsonValue const& language) {
        if (language.is_string() && Unicode::is_locale_available(language.as_string()))
            parsed_languages.append(language.as_string());
    });

    if (parsed_languages.is_empty())
        return { DEFAULT_LANGUAGE };

    return parsed_languages;
}

void Settings::set_languages(Vector<String> languages)
{
    m_languages = move(languages);
    persist_settings();

    for (auto& observer : m_observers)
        observer.languages_changed();
}

BrowsingBehavior Settings::parse_browsing_behavior(JsonValue const& settings)
{
    if (!settings.is_object())
        return {};

    BrowsingBehavior browsing_behavior;

    if (auto enable_autoscroll = settings.as_object().get_bool(ENABLE_AUTOSCROLL_KEY); enable_autoscroll.has_value())
        browsing_behavior.enable_autoscroll = *enable_autoscroll;

    return browsing_behavior;
}

void Settings::set_browsing_behavior(BrowsingBehavior browsing_behavior)
{
    m_browsing_behavior = browsing_behavior;
    persist_settings();

    for (auto& observer : m_observers)
        observer.browsing_behavior_changed();
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

    auto name = search_engine.as_object().get_string(SEARCH_ENGINE_NAME_KEY);
    auto url = search_engine.as_object().get_string(SEARCH_ENGINE_URL_KEY);
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

BrowsingDataSettings Settings::parse_browsing_data_settings(JsonValue const& settings)
{
    if (!settings.is_object())
        return {};

    BrowsingDataSettings browsing_data_settings;

    if (auto disk_cache_settings = settings.as_object().get_object(DISK_CACHE_KEY); disk_cache_settings.has_value()) {
        if (auto maximum_size = disk_cache_settings->get_integer<u64>(DISK_CACHE_MAXIMUM_SIZE_KEY); maximum_size.has_value())
            browsing_data_settings.disk_cache_settings.maximum_size = *maximum_size;
    }

    return browsing_data_settings;
}

void Settings::set_browsing_data_settings(BrowsingDataSettings browsing_data_settings)
{
    m_browsing_data_settings = browsing_data_settings;
    persist_settings();

    for (auto& observer : m_observers)
        observer.browsing_data_settings_changed();
}

void Settings::set_global_privacy_control(GlobalPrivacyControl global_privacy_control)
{
    m_global_privacy_control = global_privacy_control;
    persist_settings();

    for (auto& observer : m_observers)
        observer.global_privacy_control_changed();
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
            auto validate_dnssec_locally = dns_settings_object.get_bool("dnssec"sv);

            if (server.has_value() && port.has_value() && type.has_value()) {
                if (*type == "tls"sv)
                    return DNSOverTLS { .server_address = server->to_byte_string(), .port = *port, .validate_dnssec_locally = validate_dnssec_locally.value_or(false) };
                if (*type == "udp"sv)
                    return DNSOverUDP { .server_address = server->to_byte_string(), .port = *port, .validate_dnssec_locally = validate_dnssec_locally.value_or(false) };
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

    if (auto result = write_json_file(m_settings_path, settings); result.is_error())
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

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::BrowsingBehavior const& browsing_behavior)
{
    TRY(encoder.encode(browsing_behavior.enable_autoscroll));

    return {};
}

template<>
ErrorOr<WebView::BrowsingBehavior> decode(Decoder& decoder)
{
    auto enable_autoscroll = TRY(decoder.decode<bool>());

    return WebView::BrowsingBehavior { enable_autoscroll };
}

}
