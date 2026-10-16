/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <LibHTTP/Cache/DiskCacheSettings.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/AutoplayPolicy.h>
#include <LibWebCommon/WebView/BrowsingBehavior.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Options.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

struct WindowSettings {
    Optional<Gfx::IntPoint> position;
    Gfx::IntSize size { 1200, 900 };
    bool maximized { false };
};

enum class ShowBookmarksBar : u8 {
    Always,
    Never,
    OnHover,
};

struct Appearance {
    bool show_menu_bar { false };
    ShowBookmarksBar show_bookmarks_bar { ShowBookmarksBar::Always };
};

struct ContentSettings {
    double default_zoom_level_factor { 1.0 };
    HashMap<String, double> zoom_per_host;

    bool enable_force_dark { false };
};

enum class VerticalTabsPosition : u8 {
    Left,
    Right,
};

struct TabSettings {
    bool vertical_tabs_enabled { false };
    bool vertical_tabs_expanded { true };
    bool vertical_tabs_expand_on_hover { false };
    VerticalTabsPosition vertical_tabs_position { VerticalTabsPosition::Left };
    Optional<u16> vertical_tabs_expanded_width;
};

struct SearchEngineSettings {
    Optional<SearchEngine> engine;
    bool suggestions { false };
};

struct SiteSetting {
    OrderedHashTable<String> site_filters;

protected:
    SiteSetting() = default;
};

struct AutoplaySiteSetting : public SiteSetting {
    Web::HTML::AutoplayPolicy policy { Web::HTML::AutoplayPolicy::BlockAudio };
};

struct BrowsingDataSettings {
    HTTP::DiskCacheSettings disk_cache_settings;
};

enum class GlobalPrivacyControl {
    No,
    Yes,
};

struct ContentBlockerList {
    String identifier;
    String name;
    Optional<URL::URL> url;
    bool enabled { true };
    bool built_in { false };
    bool language_specific { false };
    String description {};
};

enum class ConfigVariableID : u8 {
    ShowWebContentProcessIDInTabTitle,
    ShowAdvancedDebugMenu,
    ShowTabPerformanceMonitor,
    UseClientSideWindowDecorations,
    VerticalTabsHoverExpandDelay,
    VerticalTabsHoverExpandAnimationDuration,
    MaximumConnectionsPerDownload,
    SplitDownloadsWithoutValidators,
    RestartStalledConnections,

    Count,
};

struct ConfigVariableDefinition {
    ConfigVariableID id;
    StringView name;
    StringView title;
    StringView description;
    JsonValue default_value;
    Optional<JsonValue::Type> array_element_type;
};

WEBVIEW_API ReadonlySpan<ConfigVariableDefinition> config_variable_definitions();
WEBVIEW_API ConfigVariableDefinition const& config_variable_definition(ConfigVariableID);
WEBVIEW_API Optional<ConfigVariableID> config_variable_id_from_name(StringView);

class WEBVIEW_API SettingsObserver {
public:
    explicit SettingsObserver();
    virtual ~SettingsObserver();

    virtual void new_tab_page_url_changed() { }
    virtual void restore_previous_session_changed() { }
    virtual void languages_changed() { }
    virtual void appearance_changed() { }
    virtual void content_settings_changed() { }
    virtual void tab_settings_changed() { }
    virtual void browsing_behavior_changed() { }
    virtual void search_engine_settings_changed() { }
    virtual void autoplay_settings_changed() { }
    virtual void browsing_data_settings_changed() { }
    virtual void global_privacy_control_changed() { }
    virtual void dns_settings_changed() { }
    virtual void config_variable_changed(ConfigVariableID) { }
    virtual void geolocation_settings_changed() { }
    virtual void background_networking_settings_changed() { }
    virtual void content_blocker_settings_changed() { }
    virtual void content_blocker_count_visibility_changed() { }

private:
    WeakPtr<Settings> m_settings;
};

class WEBVIEW_API Settings : public Weakable<Settings> {
public:
    AK_ALLOC_WITH_KMALLOC;

    // The platform's languages stand in for the user's list until the settings file carries one.
    static Settings create(ByteString settings_path, Vector<String> platform_languages = {});

    JsonValue serialize_json() const;

    bool first_run_complete() const { return m_first_run_complete; }
    ErrorOr<void> complete_first_run();

    static WindowSettings parse_window_settings(JsonValue const&);
    WindowSettings const& window_settings() const { return m_window_settings; }
    void set_window_settings(WindowSettings);

    URL::URL const& new_tab_page_url() const { return m_new_tab_page_url; }
    void set_new_tab_page_url(URL::URL);

    // Read only at launch, so a change applies at the next launch.
    bool restore_previous_session() const { return m_restore_previous_session; }
    void set_restore_previous_session(bool);

    static Vector<String> parse_json_languages(JsonValue const&);
    Vector<String> const& languages() const { return m_languages; }
    void set_languages(Vector<String>);

    static Appearance parse_appearance(JsonValue const&);
    Appearance const& appearance() const { return m_appearance; }
    void set_appearance(Appearance);

    static ContentSettings parse_content_settings(JsonValue const&);
    ContentSettings const& content_settings() const { return m_content_settings; }
    void set_content_settings(ContentSettings);

    double zoom_for_host(StringView host) const;
    void set_zoom_for_host(String const& host, double zoom_level);

    static TabSettings parse_tab_settings(JsonValue const&);
    TabSettings const& tab_settings() const { return m_tab_settings; }
    void set_tab_settings(TabSettings);

    static BrowsingBehavior parse_browsing_behavior(JsonValue const&);
    BrowsingBehavior browsing_behavior() const;
    void set_browsing_behavior(BrowsingBehavior);

    SearchEngineSettings const& search_engine_settings() const { return m_search_engine_settings; }
    void set_search_engine_settings(SearchEngineSettings);
    Optional<SearchEngine> find_search_engine_by_name(StringView name) const;

    static Optional<SearchEngine> parse_custom_search_engine(JsonValue const&);
    void add_custom_search_engine(SearchEngine);
    void remove_custom_search_engine(SearchEngine const&);

    AutoplaySiteSetting const& autoplay_settings() const { return m_autoplay; }
    void set_autoplay_policy(Web::HTML::AutoplayPolicy);
    void add_autoplay_site_filter(String const&);
    void remove_autoplay_site_filter(String const&);
    void remove_all_autoplay_site_filters();

    bool geolocation_enabled() const { return m_geolocation_enabled; }
    void set_geolocation_enabled(bool);

    static BrowsingDataSettings parse_browsing_data_settings(JsonValue const&);
    BrowsingDataSettings const& browsing_data_settings() const { return m_browsing_data_settings; }
    void set_browsing_data_settings(BrowsingDataSettings);

    GlobalPrivacyControl global_privacy_control() const { return m_global_privacy_control; }
    void set_global_privacy_control(GlobalPrivacyControl);

    bool background_networking_enabled() const { return m_background_networking_enabled; }
    void set_background_networking_enabled(bool);
    bool filter_list_updates_enabled() const { return m_filter_list_updates_enabled; }
    bool automatic_filter_list_updates_allowed() const { return m_background_networking_enabled && m_filter_list_updates_enabled; }
    void set_filter_list_updates_enabled(bool);

    bool content_blocker_enabled() const { return m_content_blocker_enabled; }
    void set_content_blocker_enabled(bool);

    bool show_content_blocker_count() const { return m_show_content_blocker_count; }
    void set_show_content_blocker_count(bool);

    HashTable<String> const& content_blocker_disabled_sites() const { return m_content_blocker_disabled_sites; }
    bool content_blocker_enabled_for_site(StringView) const;
    ErrorOr<void> set_content_blocker_enabled_for_site(StringView, bool enabled);

    Vector<ContentBlockerList> const& content_blocker_lists() const { return m_content_blocker_lists; }
    Optional<ContentBlockerList const&> content_blocker_list(StringView identifier) const;
    String add_content_blocker_list(String name, Optional<URL::URL> = {});
    void set_content_blocker_list_enabled(StringView identifier, bool);
    bool remove_content_blocker_list(StringView identifier);
    String const& custom_content_blocker_filters() const { return m_custom_content_blocker_filters; }
    void set_custom_content_blocker_filters(String);

    static DNSSettings parse_dns_settings(JsonValue const&);
    DNSSettings const& dns_settings() const { return m_dns_settings; }
    void set_dns_settings(DNSSettings const&, bool override_by_command_line = false);

    JsonValue const& config_variable(ConfigVariableID) const;
    bool config_variable_as_bool(ConfigVariableID) const;
    void set_config_variable(ConfigVariableID, JsonValue);
    void set_config_variable(StringView name, JsonValue const&);

    template<Integral T>
    T config_variable_as_integer(ConfigVariableID id) const
    {
        auto const& variable = config_variable_definition(id);
        VERIFY(variable.default_value.is_number());

        auto value = config_variable(id).get_integer<T>();
        return value.value_or_lazy_evaluated([&] { return variable.default_value.get_integer<T>().value(); });
    }

    void add_observer(Badge<SettingsObserver>, SettingsObserver&);
    void remove_observer(Badge<SettingsObserver>, SettingsObserver&);

private:
    explicit Settings(ByteString settings_path);

    void persist_settings();

    ByteString m_settings_path;
    bool m_first_run_complete { false };

    WindowSettings m_window_settings;

    URL::URL m_new_tab_page_url;
    bool m_restore_previous_session { false };
    Vector<String> m_languages;

    Appearance m_appearance;
    ContentSettings m_content_settings;
    TabSettings m_tab_settings;
    BrowsingBehavior m_browsing_behavior;

    SearchEngineSettings m_search_engine_settings;
    Vector<SearchEngine> m_custom_search_engines;

    AutoplaySiteSetting m_autoplay;
    bool m_geolocation_enabled { false };

    BrowsingDataSettings m_browsing_data_settings;
    GlobalPrivacyControl m_global_privacy_control { GlobalPrivacyControl::No };

    bool m_background_networking_enabled { true };
    bool m_filter_list_updates_enabled { false };
    bool m_content_blocker_enabled { false };
    bool m_show_content_blocker_count { false };
    HashTable<String> m_content_blocker_disabled_sites;
    Vector<ContentBlockerList> m_content_blocker_lists;
    String m_custom_content_blocker_filters;

    DNSSettings m_dns_settings { SystemDNS() };
    bool m_dns_override_by_command_line { false };

    Array<JsonValue, static_cast<size_t>(ConfigVariableID::Count)> m_config_variables {};

    Vector<SettingsObserver&> m_observers;
};

}
