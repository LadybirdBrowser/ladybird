/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/Platform.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/AutoplayPolicy.h>
#include <LibWebView/Application.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/WebUI/SettingsUI.h>

namespace WebView {

static StringView config_variable_type_to_string(JsonValue::Type type)
{
    switch (type) {
    case JsonValue::Type::Null:
        return "null"sv;
    case JsonValue::Type::Bool:
        return "boolean"sv;
    case JsonValue::Type::Number:
        return "number"sv;
    case JsonValue::Type::String:
        return "string"sv;
    case JsonValue::Type::Array:
        return "array"sv;
    case JsonValue::Type::Object:
        return "object"sv;
    }

    VERIFY_NOT_REACHED();
}

static bool should_show_config_variable([[maybe_unused]] ConfigVariableID id)
{
#if !defined(AK_OS_MACOS)
    if (id == ConfigVariableID::UseRoundedWindowCorners)
        return false;
#endif
    if (id == ConfigVariableID::UseServerSideWindowDecorations)
        return Application::the().supports_server_side_window_decorations();

    return true;
}

void SettingsUI::register_interfaces()
{
    register_interface("loadFeatures"sv, [this](auto const&) {
        load_features();
    });
    register_interface("loadCurrentSettings"sv, [this](auto const&) {
        load_current_settings();
    });

    register_interface("setNewTabPageURL"sv, [this](auto const& data) {
        set_new_tab_page_url(data);
    });
    register_interface("setTabSettings"sv, [this](auto const& data) {
        set_tab_settings(data);
    });
    register_interface("setDefaultZoomLevelFactor"sv, [this](auto const& data) {
        set_default_zoom_level_factor(data);
    });
    register_interface("setLanguages"sv, [this](auto const& data) {
        set_languages(data);
    });
    register_interface("setBrowsingBehavior"sv, [this](auto const& data) {
        set_browsing_behavior(data);
    });
    register_interface("setConfigVariable"sv, [this](auto const& data) {
        set_config_variable(data);
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
    register_interface("setSiteSettingPolicy"sv, [this](auto const& data) {
        set_site_setting_policy(data);
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

void SettingsUI::load_features()
{
    auto& application = Application::the();

    JsonObject features;
    features.set("primaryPaste"_string, application.supports_clipboard_type(Application::ClipboardType::Selection));
    features.set("verticalTabs"_string, application.supports_vertical_tabs());

    async_send_message("loadFeatures"sv, move(features));
}

void SettingsUI::load_current_settings()
{
    auto settings = WebView::Application::settings().serialize_json();

    JsonArray config_variables;
    for (auto const& variable : config_variable_definitions()) {
        if (!should_show_config_variable(variable.id))
            continue;

        JsonObject variable_object;
        variable_object.set("name"sv, variable.name);
        variable_object.set("title"sv, variable.title);
        variable_object.set("description"sv, variable.description);
        variable_object.set("type"sv, config_variable_type_to_string(variable.default_value.type()));
        if (variable.array_element_type.has_value())
            variable_object.set("elementType"sv, config_variable_type_to_string(*variable.array_element_type));
        variable_object.set("defaultValue"sv, variable.default_value);
        variable_object.set("value"sv, WebView::Application::settings().config_variable(variable.id));

        config_variables.must_append(move(variable_object));
    }

    settings.as_object().set("configVariableDefinitions"sv, move(config_variables));
    async_send_message("loadSettings"sv, settings);
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

void SettingsUI::set_tab_settings(JsonValue const& tab_settings)
{
    auto& settings = WebView::Application::settings();
    auto parsed_tab_settings = Settings::parse_tab_settings(tab_settings);
    auto const& current_tab_settings = settings.tab_settings();

    // Collapsed/expanded vertical tabs and their width are not controlled by the settings UI. Don't overwrite them.
    parsed_tab_settings.vertical_tabs_expanded = current_tab_settings.vertical_tabs_expanded;
    parsed_tab_settings.vertical_tabs_expanded_width = current_tab_settings.vertical_tabs_expanded_width;

    settings.set_tab_settings(parsed_tab_settings);
    load_current_settings();
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

void SettingsUI::set_browsing_behavior(JsonValue const& browsing_behavior)
{
    auto parsed_browsing_behavior = Settings::parse_browsing_behavior(browsing_behavior);
    WebView::Application::settings().set_browsing_behavior(parsed_browsing_behavior);

    load_current_settings();
}

void SettingsUI::set_config_variable(JsonValue const& variable)
{
    if (!variable.is_object())
        return;

    auto name = variable.as_object().get_string("name"sv);
    auto value = variable.as_object().get("value"sv);
    if (!name.has_value() || !value.has_value())
        return;

    WebView::Application::settings().set_config_variable(*name, *value);

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
        WebView::Application::settings().set_autocomplete_engine(OptionalNone {});
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
        WebView::Application::settings().set_autocomplete_engine(OptionalNone {});
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

void SettingsUI::set_site_setting_policy(JsonValue const& site_setting)
{
    auto setting = site_setting_type(site_setting);
    if (!setting.has_value())
        return;

    auto policy = site_setting.as_object().get_string("policy"sv);
    if (!policy.has_value())
        return;

    switch (*setting) {
    case SiteSettingType::Autoplay:
        if (auto parsed = Web::HTML::autoplay_policy_from_string(*policy); parsed.has_value())
            WebView::Application::settings().set_autoplay_policy(*parsed);
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
    auto weak_this = static_cast<Core::EventReceiver&>(*this).make_weak_ptr();

    auto since = [&]() {
        if (auto since = options.as_object().get_integer<i64>("since"sv); since.has_value())
            return UnixDateTime::from_milliseconds_since_epoch(*since);
        return UnixDateTime::earliest();
    }();

    application.estimate_browsing_data_size_accessed_since(since)
        ->when_resolved([weak_this](Application::BrowsingDataSizes const& sizes) {
            if (!weak_this)
                return;

            auto& settings_ui = static_cast<SettingsUI&>(*weak_this.ptr());
            if (!settings_ui.is_open())
                return;

            JsonObject result;

            result.set("cacheSizeSinceRequestedTime"sv, sizes.cache_size_since_requested_time);
            result.set("totalCacheSize"sv, sizes.total_cache_size);

            result.set("siteDataSizeSinceRequestedTime"sv, sizes.site_data_size_since_requested_time);
            result.set("totalSiteDataSize"sv, sizes.total_site_data_size);

            settings_ui.async_send_message("estimatedBrowsingDataSizes"sv, move(result));
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

    clear_browsing_data_options.delete_history = options.as_object().get_bool("history"sv).value_or(false)
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
