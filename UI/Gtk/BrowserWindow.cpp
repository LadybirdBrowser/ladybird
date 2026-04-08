/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/EventLoop.h>
#include <LibURL/Parser.h>
#include <LibWebView/Menu.h>
#include <LibWebView/URL.h>
#include <UI/Gtk/Application.h>
#include <UI/Gtk/BrowserWindow.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Menu.h>
#include <UI/Gtk/Tab.h>
#include <UI/Gtk/WebContentView.h>

namespace Ladybird {

class NavActionObserver final : public WebView::Action::Observer {
public:
    NavActionObserver(GSimpleAction* gaction)
        : m_gaction(gaction)
    {
    }

    void on_enabled_state_changed(WebView::Action& action) override
    {
        g_simple_action_set_enabled(m_gaction, action.enabled());
    }

private:
    GSimpleAction* m_gaction;
};

void BrowserWindow::ActionBinding::detach()
{
    if (action && observer)
        action->remove_observer(*observer);
    action = nullptr;
    observer = nullptr;
}

BrowserWindow::BrowserWindow(AdwApplication* app, Vector<URL::URL> const& initial_urls)
{
    setup_ui(app);
    setup_keyboard_shortcuts();

    if (initial_urls.is_empty()) {
        create_new_tab(Web::HTML::ActivateTab::Yes);
    } else {
        for (size_t i = 0; i < initial_urls.size(); ++i) {
            create_new_tab(initial_urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
        }
    }
}

BrowserWindow::~BrowserWindow()
{
    m_back_binding.detach();
    m_forward_binding.detach();
    g_signal_handlers_disconnect_by_data(m_window, this);
    if (m_location_entry)
        g_signal_handlers_disconnect_by_data(m_location_entry, this);
    m_tabs.clear();
}

void BrowserWindow::register_actions()
{
    struct ActionData {
        BrowserWindow* window;
        void (*callback)(BrowserWindow&);
    };
    auto add_action = [&](char const* name, void (*callback)(BrowserWindow&), bool enabled = true) {
        GObjectPtr action { g_simple_action_new(name, nullptr) };
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action.ptr()), enabled);
        g_signal_connect_data(action.ptr(), "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
            auto* data = static_cast<ActionData*>(user_data);
            data->callback(*data->window); }), new ActionData { this, callback }, +[](gpointer data, GClosure*) { delete static_cast<ActionData*>(data); }, static_cast<GConnectFlags>(0));
        g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(action.ptr()));
    };

    add_action("new-tab", [](BrowserWindow& self) { self.create_new_tab(Web::HTML::ActivateTab::Yes); });
    add_action("new-window", [](BrowserWindow&) { Application::the().new_window({}); });
    add_action("close-tab", [](BrowserWindow& self) { self.close_current_tab(); });
    add_action("focus-location", [](BrowserWindow& self) { ladybird_location_entry_focus_and_select_all(self.m_location_entry); });

    add_action("go-back", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().traverse_the_history_by_delta(-1); }, false);

    add_action("go-forward", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().traverse_the_history_by_delta(1); }, false);

    add_action("zoom-in", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().zoom_in(); });

    add_action("zoom-out", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().zoom_out(); });

    add_action("zoom-reset", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().reset_zoom(); }, false);

    add_action("find", [](BrowserWindow& self) { self.show_find_bar(); });
    add_action("find-close", [](BrowserWindow& self) { self.hide_find_bar(); });

    add_action("find-next", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().find_in_page_next_match(); });

    add_action("find-previous", [](BrowserWindow& self) {
        if (auto* tab = self.current_tab())
            tab->view().find_in_page_previous_match(); });

    add_action("quit", [](BrowserWindow&) { Core::EventLoop::current().quit(0); });

    add_action("fullscreen", [](BrowserWindow& self) {
        if (gtk_window_is_fullscreen(GTK_WINDOW(self.m_window)))
            gtk_window_unfullscreen(GTK_WINDOW(self.m_window));
        else
            gtk_window_fullscreen(GTK_WINDOW(self.m_window)); });

    auto& app = WebView::Application::the();
    add_action_to_map(G_ACTION_MAP(m_window), "reload", app.reload_action());
    add_action_to_map(G_ACTION_MAP(m_window), "preferences", app.open_settings_page_action());
    add_action_to_map(G_ACTION_MAP(m_window), "about", app.open_about_page_action());
}

void BrowserWindow::setup_ui(AdwApplication* app)
{
    auto* browser_window_widget = LadybirdWidgets::create_browser_window_widget(app);
    m_window = ADW_APPLICATION_WINDOW(browser_window_widget);

    m_tab_view = LadybirdWidgets::browser_window_tab_view(browser_window_widget);
    m_header_bar = LadybirdWidgets::browser_window_header_bar(browser_window_widget);
    m_restore_button = LadybirdWidgets::browser_window_restore_button(browser_window_widget);
    m_zoom_label = LadybirdWidgets::browser_window_zoom_label(browser_window_widget);
    m_devtools_banner = LadybirdWidgets::browser_window_devtools_banner(browser_window_widget);
    m_find_bar_revealer = LadybirdWidgets::browser_window_find_bar_revealer(browser_window_widget);
    m_find_entry = LadybirdWidgets::browser_window_find_entry(browser_window_widget);
    m_find_result_label = LadybirdWidgets::browser_window_find_result_label(browser_window_widget);
    m_toast_overlay = LadybirdWidgets::browser_window_toast_overlay(browser_window_widget);

    // Connect find entry signals
    g_signal_connect_swapped(m_find_entry, "search-changed", G_CALLBACK(+[](BrowserWindow* self, GtkSearchEntry* entry) {
        auto* text = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (auto* tab = self->current_tab()) {
            if (text && text[0] != '\0')
                tab->view().find_in_page(MUST(String::from_utf8(StringView(text, strlen(text)))));
        }
    }),
        this);
    g_signal_connect_swapped(m_find_entry, "activate", G_CALLBACK(+[](BrowserWindow* self, GtkSearchEntry*) {
        if (auto* tab = self->current_tab())
            tab->view().find_in_page_next_match();
    }),
        this);
    g_signal_connect_swapped(m_find_entry, "next-match", G_CALLBACK(+[](BrowserWindow* self, GtkSearchEntry*) {
        if (auto* tab = self->current_tab())
            tab->view().find_in_page_next_match();
    }),
        this);
    g_signal_connect_swapped(m_find_entry, "previous-match", G_CALLBACK(+[](BrowserWindow* self, GtkSearchEntry*) {
        if (auto* tab = self->current_tab())
            tab->view().find_in_page_previous_match();
    }),
        this);
    g_signal_connect_swapped(m_find_entry, "stop-search", G_CALLBACK(+[](BrowserWindow* self, GtkSearchEntry*) {
        self->hide_find_bar();
    }),
        this);

    register_actions();

    g_signal_connect_swapped(m_tab_view, "close-page", G_CALLBACK(+[](BrowserWindow* self, AdwTabPage* page) -> gboolean {
        self->on_tab_close_request(page);
        return GDK_EVENT_STOP;
    }),
        this);

    g_signal_connect_swapped(m_tab_view, "notify::selected-page", G_CALLBACK(+[](BrowserWindow* self, GParamSpec*) {
        self->on_tab_switched();
    }),
        this);

    // URL entry (centered title widget)
    m_location_entry = ladybird_location_entry_new();
    ladybird_location_entry_set_on_navigate(m_location_entry, [this](String url_string) {
        if (auto url = URL::Parser::basic_parse(url_string); url.has_value()) {
            if (auto* tab = current_tab())
                tab->navigate(url.release_value());
            if (auto* v = view())
                gtk_widget_grab_focus(GTK_WIDGET(v->gtk_widget()));
        }
    });

    adw_header_bar_set_title_widget(m_header_bar, GTK_WIDGET(m_location_entry));

    GObjectPtr developer_tools_submenu { g_menu_new() };
    GObjectPtr inspect_gmenu { create_application_menu(WebView::Application::the().inspect_menu(), [](WebView::Action& action) {
        return ByteString::formatted("win.inspect-{}", static_cast<int>(action.id()));
    }) };
    GObjectPtr debug_gmenu { create_application_menu(WebView::Application::the().debug_menu(), [](WebView::Action& action) {
        return ByteString::formatted("win.debug-{}", static_cast<int>(action.id()));
    }) };
    g_menu_append_section(G_MENU(developer_tools_submenu.ptr()), nullptr, G_MENU_MODEL(inspect_gmenu.ptr()));
    g_menu_append_section(G_MENU(developer_tools_submenu.ptr()), "Debug", G_MENU_MODEL(debug_gmenu.ptr()));
    append_submenu_to_section_containing_action(LadybirdWidgets::browser_window_hamburger_menu(browser_window_widget), "win.new-window", "Developer Tools", G_MENU_MODEL(developer_tools_submenu.ptr()));

    // Listen for fullscreen state changes
    g_signal_connect_swapped(m_window, "notify::fullscreened", G_CALLBACK(+[](BrowserWindow* self, GParamSpec*) {
        gboolean fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(self->m_window));
        adw_header_bar_set_show_start_title_buttons(self->m_header_bar, !fullscreen);
        adw_header_bar_set_show_end_title_buttons(self->m_header_bar, !fullscreen);
        gtk_widget_set_visible(GTK_WIDGET(self->m_restore_button), fullscreen);
    }),
        this);

    add_menu_actions_to_map(G_ACTION_MAP(m_window), WebView::Application::the().inspect_menu(), [](WebView::Action& action) {
        return ByteString::formatted("inspect-{}", static_cast<int>(action.id()));
    });
    add_menu_actions_to_map(G_ACTION_MAP(m_window), WebView::Application::the().debug_menu(), [](WebView::Action& action) {
        return ByteString::formatted("debug-{}", static_cast<int>(action.id()));
    });

    auto* application = gtk_window_get_application(GTK_WINDOW(m_window));
    install_action_accelerators(application, "win.reload", WebView::Application::the().reload_action());
    install_action_accelerators(application, "win.preferences", WebView::Application::the().open_settings_page_action());
    install_action_accelerators(application, "win.about", WebView::Application::the().open_about_page_action());
    install_menu_action_accelerators(application, "win.inspect", WebView::Application::the().inspect_menu());
    install_menu_action_accelerators(application, "win.debug", WebView::Application::the().debug_menu());

    if (WebView::Application::browser_options().devtools_port.has_value())
        on_devtools_enabled();
}

void BrowserWindow::setup_keyboard_shortcuts()
{
    auto* app = gtk_window_get_application(GTK_WINDOW(m_window));

    auto set_accels = [&](char const* action, std::initializer_list<char const*> accels) {
        Vector<char const*> list;
        list.ensure_capacity(accels.size() + 1);
        for (auto* a : accels)
            list.append(a);
        list.append(nullptr);
        gtk_application_set_accels_for_action(app, action, list.data());
    };

    set_accels("win.new-tab", { "<Ctrl>t" });
    set_accels("win.close-tab", { "<Ctrl>w" });
    set_accels("win.focus-location", { "<Ctrl>l" });
    set_accels("win.find", { "<Ctrl>f" });
    set_accels("win.find-close", { "Escape" });
    set_accels("win.go-back", { "<Alt>Left" });
    set_accels("win.go-forward", { "<Alt>Right" });
    set_accels("win.zoom-in", { "<Ctrl>equal", "<Ctrl>plus" });
    set_accels("win.zoom-out", { "<Ctrl>minus" });
    set_accels("win.zoom-reset", { "<Ctrl>0" });
    set_accels("win.fullscreen", { "F11" });
    set_accels("win.quit", { "<Ctrl>q" });
    set_accels("win.new-window", { "<Ctrl>n" });
}

void BrowserWindow::on_tab_switched()
{
    auto* tab = current_tab();
    if (!tab)
        return;

    auto const& url = tab->view().url();
    if (is_internal_url(url)) {
        ladybird_location_entry_set_text(m_location_entry, "");
    } else {
        auto url_str = url.serialize().to_byte_string();
        ladybird_location_entry_set_url(m_location_entry, url_str.characters());
    }

    bind_navigation_actions(tab->view());
    update_zoom_label();
}

Tab& BrowserWindow::create_new_tab(Web::HTML::ActivateTab activate_tab)
{
    auto& new_tab_url = WebView::Application::settings().new_tab_page_url();
    auto& tab = create_new_tab(new_tab_url, activate_tab);
    return tab;
}

Tab& BrowserWindow::create_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab)
{
    auto tab = make<Tab>(*this, url);
    auto& tab_ref = *tab;

    auto* page = adw_tab_view_append(m_tab_view, tab_ref.widget());
    adw_tab_page_set_title(page, "New Tab");
    tab_ref.set_tab_page(page);

    if (activate_tab == Web::HTML::ActivateTab::Yes) {
        adw_tab_view_set_selected_page(m_tab_view, page);
        bind_navigation_actions(tab_ref.view());

        if (is_internal_url(url)) {
            ladybird_location_entry_set_text(m_location_entry, "");
            ladybird_location_entry_focus_and_select_all(m_location_entry);
        }
    }

    m_tabs.append(move(tab));
    return tab_ref;
}

Tab& BrowserWindow::create_child_tab(Web::HTML::ActivateTab activate_tab, Tab& parent, u64 page_index)
{
    auto tab = make<Tab>(*this, parent.view().client(), page_index);
    auto& tab_ref = *tab;

    auto* page = adw_tab_view_append(m_tab_view, tab_ref.widget());
    adw_tab_page_set_title(page, "New Tab");
    tab_ref.set_tab_page(page);

    if (activate_tab == Web::HTML::ActivateTab::Yes)
        adw_tab_view_set_selected_page(m_tab_view, page);

    m_tabs.append(move(tab));
    return tab_ref;
}

void BrowserWindow::close_tab(Tab& tab)
{
    auto* page = tab.tab_page();
    if (page)
        adw_tab_view_close_page(m_tab_view, page);
}

void BrowserWindow::close_current_tab()
{
    if (auto* tab = current_tab())
        close_tab(*tab);
}

void BrowserWindow::on_tab_close_request(AdwTabPage* page)
{
    auto* child = adw_tab_page_get_child(page);
    for (auto& tab : m_tabs) {
        if (tab->widget() == child) {
            adw_tab_view_close_page_finish(m_tab_view, page, TRUE);
            m_tabs.remove_first_matching([&](auto& t) { return t.ptr() == tab.ptr(); });
            if (m_tabs.is_empty())
                gtk_window_close(GTK_WINDOW(m_window));
            return;
        }
    }
    adw_tab_view_close_page_finish(m_tab_view, page, TRUE);
}

Tab* BrowserWindow::current_tab() const
{
    auto* page = adw_tab_view_get_selected_page(m_tab_view);
    if (!page)
        return nullptr;

    auto* child = adw_tab_page_get_child(page);
    for (auto& tab : m_tabs) {
        if (tab->widget() == child)
            return tab.ptr();
    }
    return nullptr;
}

WebContentView* BrowserWindow::view() const
{
    auto* tab = current_tab();
    if (!tab)
        return nullptr;
    return &tab->view();
}

void BrowserWindow::present()
{
    gtk_window_present(GTK_WINDOW(m_window));
}

int BrowserWindow::tab_count() const
{
    return adw_tab_view_get_n_pages(m_tab_view);
}

void BrowserWindow::update_navigation_buttons(bool back_enabled, bool forward_enabled)
{
    auto* back_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_window), "go-back"));
    if (back_action)
        g_simple_action_set_enabled(back_action, back_enabled);

    auto* forward_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_window), "go-forward"));
    if (forward_action)
        g_simple_action_set_enabled(forward_action, forward_enabled);
}

void BrowserWindow::bind_navigation_actions(WebContentView& view)
{
    m_back_binding.detach();
    m_forward_binding.detach();

    auto bind = [&](ActionBinding& binding, WebView::Action& action, char const* name) {
        auto* gaction = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_window), name));
        g_simple_action_set_enabled(gaction, action.enabled());
        auto observer = make<NavActionObserver>(gaction);
        binding = { &action, observer.ptr() };
        action.add_observer(move(observer));
    };

    bind(m_back_binding, view.navigate_back_action(), "go-back");
    bind(m_forward_binding, view.navigate_forward_action(), "go-forward");
}

void BrowserWindow::update_location_entry(StringView url)
{
    if (url.is_empty()) {
        ladybird_location_entry_set_text(m_location_entry, "");
        return;
    }
    auto byte_url = ByteString(url);
    ladybird_location_entry_set_url(m_location_entry, byte_url.characters());
}

void BrowserWindow::show_find_bar()
{
    gtk_revealer_set_reveal_child(m_find_bar_revealer, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(m_find_entry));
}

void BrowserWindow::hide_find_bar()
{
    gtk_revealer_set_reveal_child(m_find_bar_revealer, FALSE);
    if (auto* v = view())
        gtk_widget_grab_focus(GTK_WIDGET(v->gtk_widget()));
}

void BrowserWindow::update_find_in_page_result(size_t current_match_index, Optional<size_t> const& total_match_count)
{
    if (total_match_count.has_value()) {
        auto text = ByteString::formatted("{} of {} matches", current_match_index + 1, total_match_count.value());
        gtk_label_set_text(m_find_result_label, text.characters());
    } else {
        gtk_label_set_text(m_find_result_label, "No matches");
    }
}

void BrowserWindow::on_devtools_enabled()
{
    auto port = WebView::Application::browser_options().devtools_port;
    auto message = ByteString::formatted("DevTools is enabled on port {}", port.value_or(0));
    adw_banner_set_title(m_devtools_banner, message.characters());
    adw_banner_set_revealed(m_devtools_banner, TRUE);

    g_signal_connect_swapped(m_devtools_banner, "button-clicked", G_CALLBACK(+[](BrowserWindow* self, AdwBanner*) {
        (void)WebView::Application::the().toggle_devtools_enabled();
        self->on_devtools_disabled();
    }),
        this);
}

void BrowserWindow::on_devtools_disabled()
{
    adw_banner_set_revealed(m_devtools_banner, FALSE);
}

bool BrowserWindow::is_internal_url(URL::URL const& url)
{
    return url.scheme().is_empty() || url == URL::about_blank() || url == URL::about_newtab();
}

void BrowserWindow::update_zoom_label()
{
    if (!m_zoom_label)
        return;
    auto* tab = current_tab();
    if (!tab)
        return;
    auto zoom = tab->view().zoom_level();
    auto text = ByteString::formatted("{}%", static_cast<int>(zoom * 100));
    gtk_label_set_text(m_zoom_label, text.characters());

    auto* action = g_action_map_lookup_action(G_ACTION_MAP(m_window), "zoom-reset");
    if (action)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), zoom != 1.0);
}

void BrowserWindow::show_toast(AdwToast* toast)
{
    if (m_toast_overlay)
        adw_toast_overlay_add_toast(m_toast_overlay, toast);
}

}
