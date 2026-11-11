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
#include <LibWebCommon/Permissions/Permissions.h>
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

// Whether something follows the system's light or dark appearance, or always uses one of them.
enum class ThemePreference : u8 {
    System,
    Light,
    Dark,
};

// The colors of the browser's own tabs, toolbars and menus. Each theme is made from one or two base colors.
#define ENUMERATE_COLOR_THEMES(X) \
    X(Neutral, "neutral")         \
    X(Harbor, "harbor")           \
    X(Glacier, "glacier")         \
    X(Lagoon, "lagoon")           \
    X(Moss, "moss")               \
    X(Saffron, "saffron")         \
    X(Copper, "copper")           \
    X(Cherry, "cherry")           \
    X(Blossom, "blossom")         \
    X(Lavender, "lavender")

enum class ColorTheme : u8 {
#define __ENUMERATE_COLOR_THEME(name, string) name,
    ENUMERATE_COLOR_THEMES(__ENUMERATE_COLOR_THEME)
#undef __ENUMERATE_COLOR_THEME
};

struct Appearance {
    bool show_menu_bar { false };
    ShowBookmarksBar show_bookmarks_bar { ShowBookmarksBar::Never };
    ThemePreference interface_theme { ThemePreference::System };
    ThemePreference website_color_scheme { ThemePreference::System };
    ColorTheme color_theme { ColorTheme::Neutral };
};

enum class HostSetting : u8 {
    Zoom,
    ForceDark,
};

struct ContentSettings {
    double default_zoom_level_factor { 1.0 };
    HashMap<String, double> zoom_per_host;

    bool enable_force_dark { false };
    HashMap<String, bool> force_dark_per_host;
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
    // With vertical tabs, the open sidebar also holds the navigation buttons, the address field and the toolbar's other
    // buttons, and there is no toolbar row. The sidebar is then always on the left.
    bool everything_in_sidebar { false };
    Optional<u16> vertical_tabs_expanded_width;
};

struct SearchEngineSettings {
    Optional<SearchEngine> engine;
    bool suggestions { false };
};

template<typename Policy, Policy default_policy>
struct SiteSetting {
    Policy policy { default_policy };
    OrderedHashMap<String, Policy> site_filters;
};

using AutoplaySiteSetting = SiteSetting<Web::HTML::AutoplayPolicy, Web::HTML::AutoplayPolicy::BlockAudio>;

enum class PermissionPolicy : u8 {
    Ask,
    Block,
};

enum class GlobalPrivacyControl {
    No,
    Yes,
};

enum class CrashReportSending {
    AskMe,
    NeverSend,
};

WEBVIEW_API CrashReportSending crash_report_sending_from_string(StringView);
WEBVIEW_API StringView crash_report_sending_to_string(CrashReportSending);

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
    ShowFeedbackButton,
    UseClientSideWindowDecorations,
    VerticalTabsHoverExpandDelay,
    VerticalTabsHoverExpandAnimationDuration,
    MaximumDiskCacheSize,
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
    explicit SettingsObserver(Settings&);
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
    virtual void permission_settings_changed(Web::Permissions::PermissionName) { }
    virtual void global_privacy_control_changed() { }
    virtual void crash_report_sending_changed() { }
    virtual void dns_settings_changed() { }
    virtual void config_variable_changed(ConfigVariableID) { }
    virtual void background_networking_settings_changed() { }
    virtual void content_blocker_settings_changed() { }

private:
    WeakPtr<Settings> m_settings;
};

// What the settings hold, which a Settings can start over from.
struct WEBVIEW_API SettingsData {
    bool m_first_run_complete { false };

    WindowSettings m_window_settings;

    URL::URL m_new_tab_page_url;
    bool m_restore_previous_session { false };
    Vector<String> m_languages;
    bool m_translation_prefers_higher_quality { false };

    Appearance m_appearance;
    ContentSettings m_content_settings;
    TabSettings m_tab_settings;
    BrowsingBehavior m_browsing_behavior;

    SearchEngineSettings m_search_engine_settings;
    Vector<SearchEngine> m_custom_search_engines;

    AutoplaySiteSetting m_autoplay;
    HashMap<Web::Permissions::PermissionName, PermissionPolicy> m_permission_policies;

    GlobalPrivacyControl m_global_privacy_control { GlobalPrivacyControl::No };
    CrashReportSending m_crash_report_sending { CrashReportSending::AskMe };

    bool m_background_networking_enabled { true };
    bool m_filter_list_updates_enabled { false };
    bool m_content_blocker_enabled { false };
    HashTable<String> m_content_blocker_disabled_sites;
    Vector<ContentBlockerList> m_content_blocker_lists;
    String m_custom_content_blocker_filters;

    DNSSettings m_dns_settings { SystemDNS() };

    Array<JsonValue, static_cast<size_t>(ConfigVariableID::Count)> m_config_variables {};
};

class WEBVIEW_API Settings
    : public Weakable<Settings>
    , private SettingsData {
public:
    AK_ALLOC_WITH_KMALLOC;

    // The platform's languages stand in for the user's list until the settings file carries one.
    static Settings create(ByteString settings_path, Vector<String> platform_languages = {});

    // Settings that live in another process, and are never read from or written to a file here.
    static Settings create_from_json(JsonValue const&);
    enum class PersistChange {
        No,
        Yes,
    };
    void replace_with_json(JsonValue const&, PersistChange);

    // Called with the settings as they are, whenever they have changed.
    Function<void(JsonValue const&)> on_persist;

    JsonValue serialize_json() const;

    // What turns one set of serialized settings into another, by top-level key. A key that is gone has the value null.
    static JsonObject changes_between(JsonObject const& from, JsonObject const& to);
    static void apply_changes(JsonObject& settings, JsonObject const& changes);

    bool first_run_complete() const { return m_first_run_complete; }
    ErrorOr<void> complete_first_run();

    static WindowSettings parse_window_settings(JsonValue const&);
    WindowSettings const& window_settings() const { return m_window_settings; }
    void set_window_settings(WindowSettings);

    static URL::URL const& default_new_tab_page_url();
    URL::URL const& new_tab_page_url() const { return m_new_tab_page_url; }
    void set_new_tab_page_url(URL::URL);

    // Read only at launch, so a change applies at the next launch.
    bool restore_previous_session() const { return m_restore_previous_session; }
    void set_restore_previous_session(bool);

    static Vector<String> parse_json_languages(JsonValue const&);
    Vector<String> const& languages() const { return m_languages; }
    void set_languages(Vector<String>);

    // Whether pages are translated with the platform's higher-quality models, rather than its faster ones.
    bool translation_prefers_higher_quality() const { return m_translation_prefers_higher_quality; }
    void set_translation_prefers_higher_quality(bool);

    static Appearance parse_appearance(JsonValue const&);
    Appearance const& appearance() const { return m_appearance; }
    void set_appearance(Appearance);

    static ContentSettings parse_content_settings(JsonValue const&);
    ContentSettings const& content_settings() const { return m_content_settings; }
    void set_content_settings(ContentSettings);

    bool force_dark_for_host(StringView host) const;
    void set_force_dark_for_host(String const& host, bool enabled);

    double zoom_for_host(StringView host) const;
    void set_zoom_for_host(String const& host, double zoom_level);

    // Omitting the host resets every override for this setting.
    void reset_host_setting(HostSetting, Optional<StringView> host = {});

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
    void add_autoplay_site_filter(String const&, Web::HTML::AutoplayPolicy);
    void remove_autoplay_site_filter(String const&);
    void remove_all_autoplay_site_filters();

    PermissionPolicy permission_policy(Web::Permissions::PermissionName) const;
    void set_permission_policy(Web::Permissions::PermissionName, PermissionPolicy);

    HTTP::DiskCacheSettings disk_cache_settings() const;

    GlobalPrivacyControl global_privacy_control() const { return m_global_privacy_control; }
    void set_global_privacy_control(GlobalPrivacyControl);

    CrashReportSending crash_report_sending() const { return m_crash_report_sending; }
    void set_crash_report_sending(CrashReportSending);

    bool background_networking_enabled() const { return m_background_networking_enabled; }
    void set_background_networking_enabled(bool);
    bool filter_list_updates_enabled() const { return m_filter_list_updates_enabled; }
    bool automatic_filter_list_updates_allowed() const { return m_background_networking_enabled && m_filter_list_updates_enabled; }
    void set_filter_list_updates_enabled(bool);

    bool content_blocker_enabled() const { return m_content_blocker_enabled; }
    void set_content_blocker_enabled(bool);

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

    void reset_to_defaults();
    void load_from_json(JsonValue const&);

    void persist_settings();

    ByteString m_settings_path;
    bool m_dns_override_by_command_line { false };

    Vector<SettingsObserver&> m_observers;
};

}
