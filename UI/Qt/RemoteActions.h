/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/WeakPtr.h>
#include <Hub/HubClientEndpoint.h>
#include <LibWebView/Menu.h>

namespace Ladybird {

// The Hub's actions and menus, as the UI shows them. Each stands in for one in the Hub, which it follows, and which it
// activates when the user picks it.
class RemoteActions {
public:
    static RemoteActions& the();

    void set_application_actions(Hub::ApplicationActions const&);

    NonnullRefPtr<WebView::Action> action_for(Hub::ActionState const&);
    NonnullRefPtr<WebView::Menu> menu_for(ReadonlySpan<Hub::MenuEntry>);
    void update(Hub::ActionState const&);
    void retire(u64 handle);

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
    static void apply_state(WebView::Action&, Hub::ActionState const&);

    HashMap<u64, WeakPtr<WebView::Action>> m_actions;

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
    RefPtr<WebView::Action> m_report_website_issue_action;

    RefPtr<WebView::Menu> m_zoom_menu;
    RefPtr<WebView::Menu> m_color_scheme_menu;
    RefPtr<WebView::Menu> m_contrast_menu;
    RefPtr<WebView::Menu> m_motion_menu;
    RefPtr<WebView::Menu> m_history_menu;
    RefPtr<WebView::Menu> m_inspect_menu;
    RefPtr<WebView::Menu> m_debug_menu;
};

}
