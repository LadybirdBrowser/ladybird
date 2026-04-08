/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/Menu.h>
#include <UI/Gtk/Widgets/LadybirdBrowserWindow.h>
#include <UI/Gtk/Widgets/LadybirdLocationEntry.h>

#include <adwaita.h>

namespace Ladybird {

class Tab;
class WebContentView;

class BrowserWindow {
public:
    BrowserWindow(AdwApplication* app, Vector<URL::URL> const& initial_urls);
    ~BrowserWindow();

    GtkWindow* gtk_window() const { return GTK_WINDOW(m_window); }

    Tab& create_new_tab(Web::HTML::ActivateTab activate_tab);
    Tab& create_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab);
    Tab& create_child_tab(Web::HTML::ActivateTab activate_tab, Tab& parent, u64 page_index);
    void close_tab(Tab& tab);
    void close_current_tab();
    Tab* current_tab() const;
    WebContentView* view() const;
    void present();
    int tab_count() const;

    void update_navigation_buttons(bool back_enabled, bool forward_enabled);
    void update_location_entry(StringView url);
    void update_zoom_label();
    void update_find_in_page_result(size_t current_match_index, Optional<size_t> const& total_match_count);

    void show_find_bar();
    void hide_find_bar();

    void on_devtools_enabled();
    void on_devtools_disabled();

    void show_toast(AdwToast* toast);

    static bool is_internal_url(URL::URL const& url);

private:
    void setup_ui(AdwApplication* app);
    void register_actions();
    void setup_keyboard_shortcuts();
    void on_tab_close_request(AdwTabPage* page);
    void on_tab_switched();
    void bind_navigation_actions(WebContentView& view);

    AdwApplicationWindow* m_window { nullptr };
    LadybirdLocationEntry* m_location_entry { nullptr };
    Vector<NonnullOwnPtr<Tab>> m_tabs;

    AdwTabView* m_tab_view { nullptr };
    AdwHeaderBar* m_header_bar { nullptr };
    GtkButton* m_restore_button { nullptr };
    GtkLabel* m_zoom_label { nullptr };
    AdwBanner* m_devtools_banner { nullptr };
    GtkRevealer* m_find_bar_revealer { nullptr };
    GtkSearchEntry* m_find_entry { nullptr };
    GtkLabel* m_find_result_label { nullptr };
    AdwToastOverlay* m_toast_overlay { nullptr };

    struct ActionBinding {
        WebView::Action* action { nullptr };
        WebView::Action::Observer* observer { nullptr };
        void detach();
    };
    ActionBinding m_back_binding;
    ActionBinding m_forward_binding;
};

}
