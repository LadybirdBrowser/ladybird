/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/Settings.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class SettingsUI;

// Tells a settings page of every change to the sites' permissions, whatever made it.
class SettingsUISitePermissionsObserver final : public PermissionManagerObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SettingsUISitePermissionsObserver(SettingsUI&);
    virtual ~SettingsUISitePermissionsObserver() override;

private:
    virtual void permissions_changed(PermissionManager const&) override;

    SettingsUI& m_settings_ui;
    WeakPtr<PermissionManager> m_manager;
};

class WEBVIEW_API SettingsUI final
    : public WebUI
    , public SettingsObserver {
    WEB_UI(SettingsUI);
    friend class SettingsUISitePermissionsObserver;

private:
    virtual void register_interfaces() override;

    virtual void new_tab_page_url_changed() override { broadcast_settings_change(); }
    virtual void restore_previous_session_changed() override { broadcast_settings_change(); }
    virtual void languages_changed() override { broadcast_settings_change(); }
    virtual void appearance_changed() override { broadcast_settings_change(); }
    virtual void content_settings_changed() override { broadcast_settings_change(); }
    virtual void tab_settings_changed() override { broadcast_settings_change(); }
    virtual void browsing_behavior_changed() override { broadcast_settings_change(); }
    virtual void search_engine_settings_changed() override { broadcast_settings_change(); }
    virtual void autoplay_settings_changed() override { broadcast_settings_change(); }
    virtual void browsing_data_settings_changed() override { broadcast_settings_change(); }
    virtual void global_privacy_control_changed() override { broadcast_settings_change(); }
    virtual void dns_settings_changed() override { broadcast_settings_change(); }
    virtual void config_variable_changed(ConfigVariableID) override { broadcast_settings_change(); }
    virtual void geolocation_settings_changed() override { broadcast_settings_change(); }
    virtual void background_networking_settings_changed() override { broadcast_settings_change(); }
    virtual void content_blocker_settings_changed() override { broadcast_settings_change(); }

    void load_features();

    JsonObject serialize_settings() const;
    void load_current_settings();
    void broadcast_settings_change();

    void complete_first_run(JsonValue const&);

    void set_new_tab_page_url(JsonValue const&);
    void set_restore_previous_session(JsonValue const&);
    void set_languages(JsonValue const&);
    void set_appearance(JsonValue const&);
    void set_content_settings(JsonValue const&);
    void set_tab_settings(JsonValue const&);
    void set_browsing_behavior(JsonValue const&);
    void set_config_variable(JsonValue const&);

    void load_available_engines();
    void set_search_engine_settings(JsonValue const&);
    void add_custom_search_engine(JsonValue const&);
    void remove_custom_search_engine(JsonValue const&);

    void load_forcibly_enabled_site_settings();
    void set_site_setting_policy(JsonValue const&);
    void add_site_setting_filter(JsonValue const&);
    void remove_site_setting_filter(JsonValue const&);
    void remove_all_site_setting_filters(JsonValue const&);
    void set_location_policy(JsonValue const&);
    void set_site_permission(JsonValue const&);

    OwnPtr<SettingsUISitePermissionsObserver> m_site_permissions_observer;

    void estimate_browsing_data_sizes(JsonValue const&);
    void set_browsing_data_settings(JsonValue const&);
    void clear_browsing_data(JsonValue const&);
    void set_global_privacy_control(JsonValue const&);

    void set_services_network_access_enabled(JsonValue const&);
    void set_filter_list_updates_enabled(JsonValue const&);
    void update_content_blocker_lists(JsonValue const&);
    void set_content_blocker_enabled(JsonValue const&);
    void set_content_blocker_enabled_for_site(JsonValue const&);
    void set_content_blocker_list_enabled(JsonValue const&);
    void add_custom_content_blocker_subscription(JsonValue const&);
    void remove_content_blocker_list(JsonValue const&);
    void import_local_content_blocker_list(JsonValue const&);
    void set_custom_content_blocker_filters(JsonValue const&);
    void report_content_blocker_result(String operation, String message);

    void set_dns_settings(JsonValue const&);

    Optional<JsonObject> m_current_settings;
    bool m_settings_change_scheduled { false };
};

}
