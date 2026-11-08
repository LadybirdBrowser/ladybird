/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Actions.h>

#include <QMetaObject>

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

    Action& reload_action() { return *m_reload_action; }
    Action& reload_bypassing_cache_action() { return *m_reload_bypassing_cache_action; }
    Action& copy_selection_action() { return *m_copy_selection_action; }
    Action& undo_action() { return *m_undo_action; }
    Action& redo_action() { return *m_redo_action; }
    Action& cut_selection_action() { return *m_cut_selection_action; }
    Action& paste_action() { return *m_paste_action; }
    Action& paste_and_match_style_action() { return *m_paste_and_match_style_action; }
    Action& select_all_action() { return *m_select_all_action; }
    Action& open_about_page_action() { return *m_open_about_page_action; }
    Action& open_downloads_page_action() { return *m_open_downloads_page_action; }
    Action& open_settings_page_action() { return *m_open_settings_page_action; }
    Action& zoom_in_action() { return *m_zoom_in_action; }
    Action& zoom_out_action() { return *m_zoom_out_action; }
    Action& reset_zoom_action() { return *m_reset_zoom_action; }
    Action& force_dark_action() { return *m_force_dark_action; }
    Action& toggle_vertical_tabs_expanded_action() { return *m_toggle_vertical_tabs_expanded_action; }
    Action& toggle_menu_bar_action() { return *m_toggle_menu_bar_action; }
    Action& view_source_action() { return *m_view_source_action; }
    Action& report_website_issue_action() { return *m_report_website_issue_action; }

    Menu& zoom_menu() { return *m_zoom_menu; }
    Menu& color_scheme_menu() { return *m_color_scheme_menu; }
    Menu& contrast_menu() { return *m_contrast_menu; }
    Menu& motion_menu() { return *m_motion_menu; }
    Menu& history_menu() { return *m_history_menu; }
    Menu& inspect_menu() { return *m_inspect_menu; }
    Menu& debug_menu() { return *m_debug_menu; }

private:
    virtual void appearance_changed() override;
    virtual void content_settings_changed() override;
    virtual void tab_settings_changed() override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;

    void create_debug_menu(PageOptions const&);
    void update_force_dark_action();
    void update_website_color_scheme_actions();
    void update_vertical_tabs_action();

    RefPtr<Action> m_reload_action;
    RefPtr<Action> m_reload_bypassing_cache_action;
    RefPtr<Action> m_copy_selection_action;
    QMetaObject::Connection m_focus_changed_connection;

    RefPtr<Action> m_undo_action;
    RefPtr<Action> m_redo_action;
    RefPtr<Action> m_cut_selection_action;
    RefPtr<Action> m_paste_action;
    RefPtr<Action> m_paste_and_match_style_action;
    RefPtr<Action> m_select_all_action;
    RefPtr<Action> m_open_about_page_action;
    RefPtr<Action> m_open_downloads_page_action;
    RefPtr<Action> m_open_settings_page_action;
    RefPtr<Action> m_zoom_in_action;
    RefPtr<Action> m_zoom_out_action;
    RefPtr<Action> m_reset_zoom_action;
    RefPtr<Action> m_force_dark_action;
    RefPtr<Action> m_toggle_vertical_tabs_expanded_action;
    RefPtr<Action> m_toggle_menu_bar_action;
    RefPtr<Action> m_view_source_action;
    RefPtr<Action> m_toggle_devtools_action;
    RefPtr<Action> m_report_website_issue_action;
    RefPtr<Action> m_show_tab_performance_monitor_action;
    RefPtr<Action> m_show_line_box_borders_action;
    RefPtr<Action> m_show_caret_hit_test_debug_overlay_action;
    RefPtr<Action> m_enable_scripting_action;
    RefPtr<Action> m_block_pop_ups_action;

    RefPtr<Menu> m_zoom_menu;
    RefPtr<Menu> m_color_scheme_menu;
    RefPtr<Menu> m_contrast_menu;
    RefPtr<Menu> m_motion_menu;
    RefPtr<Menu> m_history_menu;
    RefPtr<Menu> m_inspect_menu;
    RefPtr<Menu> m_debug_menu;
};

}
