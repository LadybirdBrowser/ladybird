/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Point.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWebView/Forward.h>
#include <UI/Gtk/Widgets/LadybirdWebView.h>

#include <adwaita.h>

namespace Ladybird {

class BrowserWindow;
class WebContentView;

class Tab {
public:
    Tab(BrowserWindow& window, URL::URL url = {});
    Tab(BrowserWindow& window, WebView::WebContentClient& parent_client, u64 page_index);
    ~Tab();

    GtkWidget* widget() const { return GTK_WIDGET(m_web_view); }
    WebContentView& view() { return *m_view; }
    WebContentView const& view() const { return *m_view; }

    void navigate(URL::URL const& url);

    AdwTabPage* tab_page() const { return m_tab_page; }
    void set_tab_page(AdwTabPage* page) { m_tab_page = page; }

private:
    Tab(BrowserWindow& window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index);
    void setup_callbacks();
    void show_select_dropdown(Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items);

    BrowserWindow& m_window;
    OwnPtr<WebContentView> m_view;

    LadybirdWebView* m_web_view { nullptr };
    AdwTabPage* m_tab_page { nullptr };

    URL::URL m_initial_url;
};

}
