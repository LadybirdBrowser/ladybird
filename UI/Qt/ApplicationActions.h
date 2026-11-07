/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/Menu.h>
#include <LibWebView/Settings.h>

namespace Ladybird {

class RemoteView;

// The actions and menus of the application, which act on the active view or on the whole browser.
class ApplicationActions final : public WebView::SettingsObserver {
    AK_ALLOC_WITH_KMALLOC;

public:
    // What the Hub had in effect for every page when this process started.
    struct PageOptions {
        String user_agent;
        Web::NavigatorCompatibilityMode navigator_compatibility_mode { Web::default_navigator_compatibility_mode };
        bool scripting_enabled { true };
        bool blocks_pop_ups { true };
        bool devtools_enabled { false };
    };

    static ApplicationActions& the();

    explicit ApplicationActions(PageOptions const&);
    virtual ~ApplicationActions() override;

    void update_for_active_view();
    void devtools_enabled_changed(bool enabled);

    WebView::Action& reload_action() { return *m_reload_action; }
    WebView::Action& reload_bypassing_cache_action() { return *m_reload_bypassing_cache_action; }
    WebView::Action& copy_selection_action() { return *m_copy_selection_action; }
    WebView::Action& undo_action() { return *m_undo_action; }
    WebView::Action& redo_action() { return *m_redo_action; }
    WebView::Action& cut_selection_action() { return *m_cut_selection_action; }
    WebView::Action& paste_action() { return *m_paste_action; }
    WebView::Action& select_all_action() { return *m_select_all_action; }
    WebView::Action& open_about_page_action() { return *m_open_about_page_action; }
    WebView::Action& open_downloads_page_action() { return *m_open_downloads_page_action; }
    WebView::Action& open_settings_page_action() { return *m_open_settings_page_action; }
    WebView::Action& zoom_in_action() { return *m_zoom_in_action; }
    WebView::Action& zoom_out_action() { return *m_zoom_out_action; }
    WebView::Action& reset_zoom_action() { return *m_reset_zoom_action; }
    WebView::Action& force_dark_action() { return *m_force_dark_action; }
    WebView::Action& toggle_vertical_tabs_expanded_action() { return *m_toggle_vertical_tabs_expanded_action; }
    WebView::Action& toggle_menu_bar_action() { return *m_toggle_menu_bar_action; }
    WebView::Action& view_source_action() { return *m_view_source_action; }
    WebView::Action& report_website_issue_action() { return *m_report_website_issue_action; }

    WebView::Menu& zoom_menu() { return *m_zoom_menu; }
    WebView::Menu& color_scheme_menu() { return *m_color_scheme_menu; }
    WebView::Menu& contrast_menu() { return *m_contrast_menu; }
    WebView::Menu& motion_menu() { return *m_motion_menu; }
    WebView::Menu& history_menu() { return *m_history_menu; }
    WebView::Menu& inspect_menu() { return *m_inspect_menu; }
    WebView::Menu& debug_menu() { return *m_debug_menu; }

private:
    virtual void appearance_changed() override;
    virtual void content_settings_changed() override;
    virtual void tab_settings_changed() override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;

    void create_debug_menu(PageOptions const&);
    void update_force_dark_action();
    void update_website_color_scheme_actions();
    void update_vertical_tabs_action();

    RefPtr<WebView::Action> m_reload_action;
    RefPtr<WebView::Action> m_reload_bypassing_cache_action;
    RefPtr<WebView::Action> m_copy_selection_action;
    RefPtr<WebView::Action> m_undo_action;
    RefPtr<WebView::Action> m_redo_action;
    RefPtr<WebView::Action> m_cut_selection_action;
    RefPtr<WebView::Action> m_paste_action;
    RefPtr<WebView::Action> m_select_all_action;
    RefPtr<WebView::Action> m_open_about_page_action;
    RefPtr<WebView::Action> m_open_downloads_page_action;
    RefPtr<WebView::Action> m_open_settings_page_action;
    RefPtr<WebView::Action> m_zoom_in_action;
    RefPtr<WebView::Action> m_zoom_out_action;
    RefPtr<WebView::Action> m_reset_zoom_action;
    RefPtr<WebView::Action> m_force_dark_action;
    RefPtr<WebView::Action> m_toggle_vertical_tabs_expanded_action;
    RefPtr<WebView::Action> m_toggle_menu_bar_action;
    RefPtr<WebView::Action> m_view_source_action;
    RefPtr<WebView::Action> m_toggle_devtools_action;
    RefPtr<WebView::Action> m_report_website_issue_action;
    RefPtr<WebView::Action> m_show_tab_performance_monitor_action;
    RefPtr<WebView::Action> m_show_line_box_borders_action;
    RefPtr<WebView::Action> m_show_caret_hit_test_debug_overlay_action;
    RefPtr<WebView::Action> m_enable_scripting_action;
    RefPtr<WebView::Action> m_block_pop_ups_action;

    RefPtr<WebView::Menu> m_zoom_menu;
    RefPtr<WebView::Menu> m_color_scheme_menu;
    RefPtr<WebView::Menu> m_contrast_menu;
    RefPtr<WebView::Menu> m_motion_menu;
    RefPtr<WebView::Menu> m_history_menu;
    RefPtr<WebView::Menu> m_inspect_menu;
    RefPtr<WebView::Menu> m_debug_menu;
};

}
