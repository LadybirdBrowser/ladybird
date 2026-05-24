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
#include <LibWebView/Settings.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Widgets/LadybirdWebView.h>

#include <adwaita.h>

namespace Ladybird {

class BrowserWindow;
class WebContentView;

class Tab : public WebView::SettingsObserver {
public:
    Tab(BrowserWindow& window, URL::URL url = {});
    Tab(BrowserWindow& window, WebView::WebContentClient& parent_client, u64 page_index);
    virtual ~Tab() override;

    GtkWidget* widget() const { return GTK_WIDGET(m_web_view); }
    WebContentView& view() { return *m_view; }
    WebContentView const& view() const { return *m_view; }

    void navigate(URL::URL const& url);
    GdkPaintable* favicon() const { return m_favicon.ptr(); }
    bool is_loading() const { return m_is_loading; }

    AdwTabPage* tab_page() const { return m_tab_page; }
    void set_tab_page(AdwTabPage*);

private:
    Tab(BrowserWindow& window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index);
    void setup_callbacks();
    void update_tab_title();
    ByteString tab_title() const;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;
    void show_select_dropdown(Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items);

    BrowserWindow& m_window;
    OwnPtr<WebContentView> m_view;

    LadybirdWebView* m_web_view { nullptr };
    AdwTabPage* m_tab_page { nullptr };

    URL::URL m_initial_url;
    ByteString m_title { "New Tab" };
    GObjectPtr<GdkPaintable> m_favicon;
    bool m_is_loading { false };
};

}
