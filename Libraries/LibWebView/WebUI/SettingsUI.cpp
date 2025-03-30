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

    register_interface("loadAvailableEngines"sv, [this](auto const&) {
        load_available_engines();
    });
    register_interface("setSearchEngine"sv, [this](auto const& data) {
        set_search_engine(data);
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

void SettingsUI::load_available_engines()
{
    JsonArray search_engines;
    for (auto const& engine : WebView::search_engines())
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

}
