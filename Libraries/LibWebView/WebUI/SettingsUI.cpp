/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/WebUI/SettingsUI.h>

namespace WebView {

void SettingsUI::register_interfaces()
{
    register_interface("loadCurrentSettings"sv, [this](auto const&) {
        load_current_settings();
    });
    register_interface("restoreDefaultSettings"sv, [this](auto const&) {
        restore_default_settings();
    });

    register_interface("setNewTabPageURL"sv, [this](auto const& data) {
        set_new_tab_page_url(data);
    });
    register_interface("setLanguages"sv, [this](auto const& data) {
        set_languages(data);
    });

    register_interface("loadAvailableEngines"sv, [this](auto const&) {
        load_available_engines();
    });
    register_interface("setSearchEngine"sv, [this](auto const& data) {
        set_search_engine(data);
    });
    register_interface("addCustomSearchEngine"sv, [this](auto const& data) {
        add_custom_search_engine(data);
    });
    register_interface("removeCustomSearchEngine"sv, [this](auto const& data) {
        remove_custom_search_engine(data);
    });
    register_interface("setAutocompleteEngine"sv, [this](auto const& data) {
        set_autocomplete_engine(data);
    });

    register_interface("loadForciblyEnabledSiteSettings"sv, [this](auto const&) {
        load_forcibly_enabled_site_settings();
    });
    register_interface("setSiteSettingEnabledGlobally"sv, [this](auto const& data) {
        set_site_setting_enabled_globally(data);
    });
    register_interface("addSiteSettingFilter"sv, [this](auto const& data) {
        add_site_setting_filter(data);
    });
    register_interface("removeSiteSettingFilter"sv, [this](auto const& data) {
        remove_site_setting_filter(data);
    });
    register_interface("removeAllSiteSettingFilters"sv, [this](auto const& data) {
        remove_all_site_setting_filters(data);
    });

    register_interface("setDoNotTrack"sv, [this](auto const& data) {
        set_do_not_track(data);
    });

    register_interface("setDNSSettings"sv, [this](auto const& data) {
        set_dns_settings(data);
    });
    register_interface("loadDNSSettings"sv, [this](auto const&) {
        load_dns_settings();
    });
}

void SettingsUI::load_current_settings()
{
    auto settings = WebView::Application::settings().serialize_json();
    async_send_message("loadSettings"sv, settings);
}

void SettingsUI::restore_default_settings()
{
    WebView::Application::settings().restore_defaults();
    load_current_settings();
}

void SettingsUI::set_new_tab_page_url(JsonValue const& new_tab_page_url)
{
    if (!new_tab_page_url.is_string())
        return;

    auto parsed_new_tab_page_url = URL::Parser::basic_parse(new_tab_page_url.as_string());
    if (!parsed_new_tab_page_url.has_value())
        return;

    WebView::Application::settings().set_new_tab_page_url(parsed_new_tab_page_url.release_value());
}

void SettingsUI::set_languages(JsonValue const& languages)
{
    auto parsed_languages = Settings::parse_json_languages(languages);
    WebView::Application::settings().set_languages(move(parsed_languages));

    load_current_settings();
}

void SettingsUI::load_available_engines()
{
    JsonArray search_engines;
    for (auto const& engine : WebView::builtin_search_engines())
        search_engines.must_append(engine.name);

    JsonArray autocomplete_engines;
    for (auto const& engine : WebView::autocomplete_engines())
        autocomplete_engines.must_append(engine.name);

    JsonObject engines;
    engines.set("search"sv, move(search_engines));
    engines.set("autocomplete"sv, move(autocomplete_engines));

    async_send_message("loadEngines"sv, move(engines));
}

void SettingsUI::set_search_engine(JsonValue const& search_engine)
{
    if (search_engine.is_null())
        WebView::Application::settings().set_search_engine({});
    else if (search_engine.is_string())
        WebView::Application::settings().set_search_engine(search_engine.as_string());
}

void SettingsUI::add_custom_search_engine(JsonValue const& search_engine)
{
    if (auto custom_engine = Settings::parse_custom_search_engine(search_engine); custom_engine.has_value())
        WebView::Application::settings().add_custom_search_engine(custom_engine.release_value());

    load_current_settings();
}

void SettingsUI::remove_custom_search_engine(JsonValue const& search_engine)
{
    if (auto custom_engine = Settings::parse_custom_search_engine(search_engine); custom_engine.has_value())
        WebView::Application::settings().remove_custom_search_engine(*custom_engine);

    load_current_settings();
}

void SettingsUI::set_autocomplete_engine(JsonValue const& autocomplete_engine)
{
    if (autocomplete_engine.is_null())
        WebView::Application::settings().set_autocomplete_engine({});
    else if (autocomplete_engine.is_string())
        WebView::Application::settings().set_autocomplete_engine(autocomplete_engine.as_string());
}

enum class SiteSettingType {
    Autoplay,
};

static constexpr StringView site_setting_type_to_string(SiteSettingType setting)
{
    switch (setting) {
    case SiteSettingType::Autoplay:
        return "autoplay"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<SiteSettingType> site_setting_type(JsonValue const& settings)
{
    if (!settings.is_object())
        return {};

    auto setting_type = settings.as_object().get_string("setting"sv);
    if (!setting_type.has_value())
        return {};

    if (*setting_type == "autoplay"sv)
        return SiteSettingType::Autoplay;
    return {};
}

void SettingsUI::load_forcibly_enabled_site_settings()
{
    JsonArray site_settings;

    if (Application::web_content_options().enable_autoplay == EnableAutoplay::Yes)
        site_settings.must_append(site_setting_type_to_string(SiteSettingType::Autoplay));

    async_send_message("forciblyEnableSiteSettings"sv, move(site_settings));
}

void SettingsUI::set_site_setting_enabled_globally(JsonValue const& site_setting)
{
    auto setting = site_setting_type(site_setting);
    if (!setting.has_value())
        return;

    auto enabled = site_setting.as_object().get_bool("enabled"sv);
    if (!enabled.has_value())
        return;

    switch (*setting) {
    case SiteSettingType::Autoplay:
        WebView::Application::settings().set_autoplay_enabled_globally(*enabled);
        break;
    }

    load_current_settings();
}

void SettingsUI::add_site_setting_filter(JsonValue const& site_setting)
{
    auto setting = site_setting_type(site_setting);
    if (!setting.has_value())
        return;

    auto filter = site_setting.as_object().get_string("filter"sv);
    if (!filter.has_value())
        return;

    switch (*setting) {
    case SiteSettingType::Autoplay:
        WebView::Application::settings().add_autoplay_site_filter(*filter);
        break;
    }

    load_current_settings();
}

void SettingsUI::remove_site_setting_filter(JsonValue const& site_setting)
{
    auto setting = site_setting_type(site_setting);
    if (!setting.has_value())
        return;

    auto filter = site_setting.as_object().get_string("filter"sv);
    if (!filter.has_value())
        return;

    switch (*setting) {
    case SiteSettingType::Autoplay:
        WebView::Application::settings().remove_autoplay_site_filter(*filter);
        break;
    }

    load_current_settings();
}

void SettingsUI::remove_all_site_setting_filters(JsonValue const& site_setting)
{
    auto setting = site_setting_type(site_setting);
    if (!setting.has_value())
        return;

    switch (*setting) {
    case SiteSettingType::Autoplay:
        WebView::Application::settings().remove_all_autoplay_site_filters();
        break;
    }

    load_current_settings();
}

void SettingsUI::set_do_not_track(JsonValue const& do_not_track)
{
    if (!do_not_track.is_bool())
        return;

    WebView::Application::settings().set_do_not_track(do_not_track.as_bool() ? DoNotTrack::Yes : DoNotTrack::No);
}

void SettingsUI::set_dns_settings(JsonValue const& dns_settings)
{
    // dnsSettings :: { mode: "system" } | { mode: "custom", server: string, port: u16, type: "udp" | "tls", forciblyEnabled: bool }
    if (!dns_settings.is_object())
        return;

    auto& obj = dns_settings.as_object();
    auto mode = obj.get_string("mode"sv);
    if (!mode.has_value())
        return;

    if (*mode == "system") {
        WebView::Application::settings().set_dns_settings(SystemDNS {});
    } else if (*mode == "custom") {
        auto server = obj.get_string("server"sv).map([](auto const& name) { return name.to_byte_string(); });
        auto port_value = obj.get("port"sv);
        auto type = obj.get_string("type"sv);
        if (!server.has_value() || !port_value.has_value() || !type.has_value())
            return;
        auto const port = port_value->get_integer<u16>().value_or(0);
        if (*type == "udp"sv) {
            WebView::Application::settings().set_dns_settings(DNSOverUDP { .server_address = *server, .port = port });
        } else if (*type == "tls"sv) {
            WebView::Application::settings().set_dns_settings(DNSOverTLS { .server_address = *server, .port = port });
        }
    }

    load_current_settings();
}

void SettingsUI::load_dns_settings()
{
    // FIXME: Don't serialize the whole thing just to get the DNS settings.
    auto settings = WebView::Application::settings().serialize_json();
    async_send_message("loadDNSSettings"sv, settings.as_object().get_object("dnsSettings"sv).release_value());
}
}
