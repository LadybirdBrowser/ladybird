/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
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
    register_interface("setDefaultZoomLevelFactor"sv, [this](auto const& data) {
        set_default_zoom_level_factor(data);
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

    register_interface("estimateBrowsingDataSizes"sv, [this](auto const& data) {
        estimate_browsing_data_sizes(data);
    });
    register_interface("setBrowsingDataSettings"sv, [this](auto const& data) {
        set_browsing_data_settings(data);
    });
    register_interface("clearBrowsingData"sv, [this](auto const& data) {
        clear_browsing_data(data);
    });
    register_interface("setGlobalPrivacyControl"sv, [this](auto const& data) {
        set_global_privacy_control(data);
    });

    register_interface("setDNSSettings"sv, [this](auto const& data) {
        set_dns_settings(data);
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

void SettingsUI::set_default_zoom_level_factor(JsonValue const& default_zoom_level_factor)
{
    auto const maybe_factor = default_zoom_level_factor.get_double_with_precision_loss();
    if (!maybe_factor.has_value())
        return;

    WebView::Application::settings().set_default_zoom_level_factor(maybe_factor.value());
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
    if (search_engine.is_null()) {
        WebView::Application::settings().set_search_engine({});
        WebView::Application::settings().set_autocomplete_engine({});
    } else if (search_engine.is_string()) {
        WebView::Application::settings().set_search_engine(search_engine.as_string());
    }

    load_current_settings();
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

void SettingsUI::estimate_browsing_data_sizes(JsonValue const& options)
{
    if (!options.is_object())
        return;

    auto& application = Application::the();

    auto since = [&]() {
        if (auto since = options.as_object().get_integer<i64>("since"sv); since.has_value())
            return UnixDateTime::from_milliseconds_since_epoch(*since);
        return UnixDateTime::earliest();
    }();

    application.estimate_browsing_data_size_accessed_since(since)
        ->when_resolved([this](Application::BrowsingDataSizes sizes) {
            JsonObject result;

            result.set("cacheSizeSinceRequestedTime"sv, sizes.cache_size_since_requested_time);
            result.set("totalCacheSize"sv, sizes.total_cache_size);

            result.set("siteDataSizeSinceRequestedTime"sv, sizes.site_data_size_since_requested_time);
            result.set("totalSiteDataSize"sv, sizes.total_site_data_size);

            async_send_message("estimatedBrowsingDataSizes"sv, move(result));
        })
        .when_rejected([](Error const& error) {
            dbgln("Failed to estimate browsing data sizes: {}", error);
        });
}

void SettingsUI::set_browsing_data_settings(JsonValue const& settings)
{
    Application::settings().set_browsing_data_settings(Settings::parse_browsing_data_settings(settings));
    load_current_settings();
}

void SettingsUI::clear_browsing_data(JsonValue const& options)
{
    if (!options.is_object())
        return;

    Application::ClearBrowsingDataOptions clear_browsing_data_options;

    if (auto since = options.as_object().get_integer<i64>("since"sv); since.has_value())
        clear_browsing_data_options.since = UnixDateTime::from_milliseconds_since_epoch(*since);

    clear_browsing_data_options.delete_cached_files = options.as_object().get_bool("cachedFiles"sv).value_or(false)
        ? Application::ClearBrowsingDataOptions::Delete::Yes
        : Application::ClearBrowsingDataOptions::Delete::No;

    clear_browsing_data_options.delete_site_data = options.as_object().get_bool("siteData"sv).value_or(false)
        ? Application::ClearBrowsingDataOptions::Delete::Yes
        : Application::ClearBrowsingDataOptions::Delete::No;

    Application::the().clear_browsing_data(clear_browsing_data_options);
}

void SettingsUI::set_global_privacy_control(JsonValue const& global_privacy_control)
{
    if (!global_privacy_control.is_bool())
        return;

    WebView::Application::settings().set_global_privacy_control(global_privacy_control.as_bool() ? GlobalPrivacyControl::Yes : GlobalPrivacyControl::No);
}

void SettingsUI::set_dns_settings(JsonValue const& dns_settings)
{
    Application::settings().set_dns_settings(Settings::parse_dns_settings(dns_settings));
    load_current_settings();
}

}
