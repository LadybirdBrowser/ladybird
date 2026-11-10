/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/InternalURLs.h>
#include <LibWebCommon/CSS/PreferredContrast.h>
#include <LibWebCommon/CSS/PreferredMotion.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/UserAgent.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/ApplicationActions.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/Tab.h>

#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QPlainTextEdit>

namespace Ladybird {

static ApplicationActions* s_the;

ApplicationActions& ApplicationActions::the()
{
    return *s_the;
}

static RemoteView* active_view()
{
    auto* tab = Application::the().active_tab();
    if (!tab || !tab->is_loaded())
        return nullptr;
    return &tab->view();
}

template<typename Callback>
static Function<void()> on_active_view(Callback callback)
{
    return [callback = move(callback)] {
        if (auto* view = active_view())
            callback(*view);
    };
}

static bool is_text_field(QWidget const* widget)
{
    return qobject_cast<QLineEdit const*>(widget) || qobject_cast<QPlainTextEdit const*>(widget);
}

// NB: Menu bar actions don't reach a focused text field in the browser chrome on their own. Run them on that field
//     instead of on the page behind it.
template<typename TextFieldCallback, typename ViewCallback>
static Function<void()> on_focused_text_field_or_active_view(TextFieldCallback text_field_callback, ViewCallback view_callback)
{
    return [text_field_callback = move(text_field_callback), view_callback = move(view_callback)] {
        auto* focus_widget = QApplication::focusWidget();
        if (auto* line_edit = qobject_cast<QLineEdit*>(focus_widget)) {
            text_field_callback(*line_edit);
            return;
        }
        if (auto* text_edit = qobject_cast<QPlainTextEdit*>(focus_widget)) {
            text_field_callback(*text_edit);
            return;
        }
        if (auto* view = active_view())
            view_callback(*view);
    };
}

static constexpr auto undo_text_field = [](auto& text_field) { text_field.undo(); };
static constexpr auto redo_text_field = [](auto& text_field) { text_field.redo(); };
static constexpr auto copy_text_field = [](auto& text_field) { text_field.copy(); };
static constexpr auto cut_text_field = [](auto& text_field) { text_field.cut(); };
static constexpr auto paste_text_field = [](auto& text_field) {
    // NB: With no text on the clipboard, QLineEdit::paste() would delete the selected text.
    if (!QApplication::clipboard()->text().isEmpty())
        text_field.paste();
};
static constexpr auto paste_and_match_style_text_field = [](auto& text_field) {
    // NB: QPlainTextEdit::paste() formats Markdown from the clipboard, so insert its plain text directly instead.
    if constexpr (IsSame<decltype(text_field), QPlainTextEdit&>) {
        if (auto text = QApplication::clipboard()->text(); !text.isEmpty())
            text_field.insertPlainText(text);
    } else {
        paste_text_field(text_field);
    }
};
static constexpr auto select_all_text_field = [](auto& text_field) { text_field.selectAll(); };

static Function<void()> debug_request(StringView request)
{
    return on_active_view([request](RemoteView& view) {
        HubConnection::the().async_view_debug_request(view.view_id(), request, {});
    });
}

ApplicationActions::ApplicationActions(PageOptions const& options)
    : WebView::SettingsObserver(Application::settings())
{
    VERIFY(!s_the);
    s_the = this;

    m_reload_action = Action::create("Reload"sv, ActionID::Reload, on_active_view([](RemoteView& view) {
        view.reload();
    }));
    m_reload_bypassing_cache_action = Action::create("Reload Without Cache"sv, ActionID::ReloadBypassingCache, on_active_view([](RemoteView& view) {
        view.reload(WebView::BypassCache::Yes);
    }));

    m_undo_action = Action::create("Undo"sv, ActionID::Undo, on_focused_text_field_or_active_view(undo_text_field, [](RemoteView& view) {
        HubConnection::the().async_view_undo(view.view_id());
    }));
    m_redo_action = Action::create("Redo"sv, ActionID::Redo, on_focused_text_field_or_active_view(redo_text_field, [](RemoteView& view) {
        HubConnection::the().async_view_redo(view.view_id());
    }));
    m_copy_selection_action = Action::create("Copy"sv, ActionID::CopySelection, on_focused_text_field_or_active_view(copy_text_field, [](RemoteView& view) {
        view.selected_text()->when_resolved([](ByteString const& text) {
            if (!text.is_empty())
                Application::the().set_clipboard_text(MUST(String::from_byte_string(text)));
        });
    }));
    m_cut_selection_action = Action::create("Cut"sv, ActionID::CutSelection, on_focused_text_field_or_active_view(cut_text_field, [](RemoteView& view) {
        HubConnection::the().async_view_cut_selection(view.view_id());
    }));
    m_paste_action = Action::create("Paste"sv, ActionID::Paste, on_focused_text_field_or_active_view(paste_text_field, [](RemoteView& view) {
        HubConnection::the().async_view_paste(view.view_id(), Web::Clipboard::PasteMode::AllMimeTypes);
    }));
    m_paste_and_match_style_action = Action::create("Paste and Match Style"sv, ActionID::PasteAndMatchStyle, on_focused_text_field_or_active_view(paste_and_match_style_text_field, [](RemoteView& view) {
        HubConnection::the().async_view_paste(view.view_id(), Web::Clipboard::PasteMode::PlainTextOnly);
    }));
    m_select_all_action = Action::create("Select All"sv, ActionID::SelectAll, on_focused_text_field_or_active_view(select_all_text_field, [](RemoteView& view) {
        HubConnection::the().async_view_select_all(view.view_id());
    }));

    m_open_about_page_action = Action::create("About Ladybird"sv, ActionID::OpenAboutPage, [] {
        Application::the().activate_or_open_tab_with_url(URL::about_version());
    });
    m_open_downloads_page_action = Action::create("Downloads"sv, ActionID::ViewDownloads, [] {
        Application::the().activate_or_open_tab_with_url(URL::about_downloads());
    });
    m_open_settings_page_action = Action::create("Settings"sv, ActionID::OpenSettingsPage, [] {
        Application::the().activate_or_open_tab_with_url(URL::about_settings());
    });

    m_zoom_in_action = Action::create("Zoom In"sv, ActionID::ZoomIn, on_active_view([](RemoteView& view) {
        view.zoom_in();
    }));
    m_zoom_out_action = Action::create("Zoom Out"sv, ActionID::ZoomOut, on_active_view([](RemoteView& view) {
        view.zoom_out();
    }));
    m_reset_zoom_action = Action::create("Reset Zoom"sv, ActionID::ResetZoom, on_active_view([](RemoteView& view) {
        view.reset_zoom();
    }));

    m_zoom_menu = Menu::create_group("Zoom"sv);
    m_zoom_menu->add_action(*m_zoom_in_action);
    m_zoom_menu->add_action(*m_zoom_out_action);
    m_zoom_menu->add_action(*m_reset_zoom_action);

    m_force_dark_action = Action::create_checkable("Force Dark Mode"sv, ActionID::ForceDark, on_active_view([](RemoteView& view) {
        if (view.is_private() == WebView::IsPrivate::Yes || view.current_host_for_settings().is_empty())
            return;
        auto& settings = Application::settings();
        settings.set_force_dark_for_host(view.current_host_for_settings(), !settings.force_dark_for_host(view.current_host_for_settings()));
    }));

    auto set_website_color_scheme = [](WebView::ThemePreference color_scheme) {
        return [color_scheme] {
            auto appearance = Application::settings().appearance();
            appearance.website_color_scheme = color_scheme;
            Application::settings().set_appearance(appearance);
        };
    };

    m_color_scheme_menu = Menu::create_group("Color Scheme"sv);
    m_color_scheme_menu->add_action(Action::create_checkable("Auto"sv, ActionID::PreferredColorScheme, set_website_color_scheme(WebView::ThemePreference::System)));
    m_color_scheme_menu->add_action(Action::create_checkable("Dark"sv, ActionID::PreferredColorScheme, set_website_color_scheme(WebView::ThemePreference::Dark)));
    m_color_scheme_menu->add_action(Action::create_checkable("Light"sv, ActionID::PreferredColorScheme, set_website_color_scheme(WebView::ThemePreference::Light)));
    update_website_color_scheme_actions();

    auto set_contrast = [](Web::CSS::PreferredContrast contrast) {
        return [contrast] { HubConnection::the().async_set_preferred_contrast(contrast); };
    };

    m_contrast_menu = Menu::create_group("Contrast"sv);
    m_contrast_menu->add_action(Action::create_checkable("Auto"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::Auto)));
    m_contrast_menu->add_action(Action::create_checkable("Less"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::Less)));
    m_contrast_menu->add_action(Action::create_checkable("More"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::More)));
    m_contrast_menu->add_action(Action::create_checkable("No Preference"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::NoPreference)));
    m_contrast_menu->items().first().get<NonnullRefPtr<Action>>()->set_checked(true);

    auto set_motion = [](Web::CSS::PreferredMotion motion) {
        return [motion] { HubConnection::the().async_set_preferred_motion(motion); };
    };

    m_motion_menu = Menu::create_group("Motion"sv);
    m_motion_menu->add_action(Action::create_checkable("Auto"sv, ActionID::PreferredMotion, set_motion(Web::CSS::PreferredMotion::Auto)));
    m_motion_menu->add_action(Action::create_checkable("Reduce"sv, ActionID::PreferredMotion, set_motion(Web::CSS::PreferredMotion::Reduce)));
    m_motion_menu->add_action(Action::create_checkable("No Preference"sv, ActionID::PreferredMotion, set_motion(Web::CSS::PreferredMotion::NoPreference)));
    m_motion_menu->items().first().get<NonnullRefPtr<Action>>()->set_checked(true);

    m_toggle_vertical_tabs_expanded_action = Action::create("Show Sidebar"sv, ActionID::ToggleVerticalTabsExpanded, [] {
        auto tab_settings = Application::settings().tab_settings();
        tab_settings.vertical_tabs_expanded = !tab_settings.vertical_tabs_expanded;
        Application::settings().set_tab_settings(tab_settings);
    });
    update_vertical_tabs_action();

    m_toggle_menu_bar_action = Action::create_checkable("Show Menu Bar"sv, ActionID::ToggleMenuBar, [] {
        auto appearance = Application::settings().appearance();
        appearance.show_menu_bar = !appearance.show_menu_bar;
        Application::settings().set_appearance(appearance);
    });
    m_toggle_menu_bar_action->set_checked(Application::settings().appearance().show_menu_bar);

    m_history_menu = Menu::create("History"sv);
    m_history_menu->add_action(Action::create("View History"sv, ActionID::ViewHistory, [] {
        Application::the().activate_or_open_tab_with_url(URL::about_history());
    }));
    m_history_menu->add_separator();
    m_history_menu->add_action(Action::create("Clear Browsing Data"sv, ActionID::ClearBrowsingData, [] {
        auto url = URL::about_settings();
        url.set_fragment("clearBrowsingData"sv);
        Application::the().activate_or_open_tab_with_url(url);
    }));

    m_inspect_menu = Menu::create("Inspect"sv);

    m_view_source_action = Action::create("View Page Source"sv, ActionID::ViewSource, on_active_view([](RemoteView& view) {
        HubConnection::the().async_view_get_source(view.view_id());
    }));
    m_inspect_menu->add_action(*m_view_source_action);

    m_toggle_devtools_action = Action::create("Enable DevTools"sv, ActionID::ToggleDevTools, [] {
        HubConnection::the().async_toggle_devtools_enabled();
    });
    m_inspect_menu->add_action(*m_toggle_devtools_action);
    devtools_enabled_changed(options.devtools_enabled);

    m_report_website_issue_action = Action::create("Report Website Issue…"sv, ActionID::ReportWebsiteIssue, on_active_view([](RemoteView& view) {
        view.report_website_issue();
    }));

    create_debug_menu(options);
    update_for_active_view();

    m_focus_changed_connection = QObject::connect(qApp, &QApplication::focusChanged, qApp, [this] {
        update_for_active_view();
    });
}

ApplicationActions::~ApplicationActions()
{
    QObject::disconnect(m_focus_changed_connection);
    s_the = nullptr;
}

void ApplicationActions::create_debug_menu(PageOptions const& options)
{
    auto& settings = Application::settings();

    m_debug_menu = Menu::create("Debug"sv);
    m_debug_menu->set_visible(settings.config_variable_as_bool(WebView::ConfigVariableID::ShowAdvancedDebugMenu));
    m_debug_menu->add_action(Action::create("Dump Session History Tree"sv, ActionID::DumpSessionHistoryTree, debug_request("dump-session-history"sv)));
    m_debug_menu->add_action(Action::create("Dump DOM Tree"sv, ActionID::DumpDOMTree, debug_request("dump-dom-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Layout Tree"sv, ActionID::DumpLayoutTree, debug_request("dump-layout-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Stacking Context Tree"sv, ActionID::DumpStackingContextTree, debug_request("dump-stacking-context-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Site Isolation Process Tree"sv, ActionID::DumpSiteIsolationProcessTree, debug_request("dump-site-isolation-process-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Display List"sv, ActionID::DumpDisplayList, debug_request("dump-display-list"sv)));
    m_debug_menu->add_action(Action::create("Dump Style Sheets"sv, ActionID::DumpStyleSheets, debug_request("dump-style-sheets"sv)));
    m_debug_menu->add_action(Action::create("Dump All Resolved Styles"sv, ActionID::DumpStyles, debug_request("dump-all-resolved-styles"sv)));
    m_debug_menu->add_action(Action::create("Dump CSS Errors"sv, ActionID::DumpCSSErrors, debug_request("dump-all-css-errors"sv)));
    m_debug_menu->add_action(Action::create("Dump Cookies"sv, ActionID::DumpCookies, [] {
        HubConnection::the().async_dump_cookies();
    }));
    m_debug_menu->add_action(Action::create("Dump Local Storage"sv, ActionID::DumpLocalStorage, debug_request("dump-local-storage"sv)));
    m_debug_menu->add_action(Action::create("Dump Session Storage"sv, ActionID::DumpSessionStorage, debug_request("dump-session-storage"sv)));
    m_debug_menu->add_action(Action::create("Dump GC Graph"sv, ActionID::DumpGCGraph, on_active_view([](RemoteView& view) {
        HubConnection::the().async_view_dump_gc_graph(view.view_id());
    })));
    m_debug_menu->add_separator();

    auto add_switch = [&](RefPtr<Action>& action, StringView name, ActionID id, bool checked, void (*set)(bool)) {
        action = Action::create_checkable(name, id, [&action, set] { set(action->checked()); });
        action->set_checked(checked);
        m_debug_menu->add_action(*action);
    };

    add_switch(m_show_line_box_borders_action, "Show Line Box Borders"sv, ActionID::ShowLineBoxBorders, false, [](bool shows) {
        HubConnection::the().async_set_shows_line_box_borders(shows);
    });
    add_switch(m_show_caret_hit_test_debug_overlay_action, "Show Caret Hit Test Debug Overlay"sv, ActionID::ShowCaretHitTestDebugOverlay, false, [](bool shows) {
        HubConnection::the().async_set_shows_caret_hit_test_debug_overlay(shows);
    });

    m_show_tab_performance_monitor_action = Action::create_checkable("Show Tab Performance Monitor"sv, ActionID::ShowTabPerformanceMonitor, [this] {
        Application::settings().set_config_variable(WebView::ConfigVariableID::ShowTabPerformanceMonitor, m_show_tab_performance_monitor_action->checked());
    });
    m_show_tab_performance_monitor_action->set_checked(settings.config_variable_as_bool(WebView::ConfigVariableID::ShowTabPerformanceMonitor));
    m_debug_menu->add_action(*m_show_tab_performance_monitor_action);
    m_debug_menu->add_separator();

    m_debug_menu->add_action(Action::create("Report Leaked Documents"sv, ActionID::ReportLeakedDocuments, debug_request("report-leaked-documents"sv)));
    m_debug_menu->add_action(Action::create("Collect Garbage"sv, ActionID::CollectGarbage, debug_request("collect-garbage"sv)));
    m_debug_menu->add_action(Action::create("Crash Current Page"sv, ActionID::CrashCurrentPage, debug_request("crash-current-page"sv)));
    m_debug_menu->add_action(Action::create("Crash Compositor Process"sv, ActionID::CrashCompositorProcess, [] {
        HubConnection::the().async_crash_compositor_process();
    }));
    m_debug_menu->add_separator();

    auto spoof_user_agent_menu = Menu::create_group("Spoof User Agent"sv);
    auto add_user_agent = [&](StringView name, StringView user_agent) {
        auto action = Action::create_checkable(name, ActionID::SpoofUserAgent, [user_agent] {
            HubConnection::the().async_set_user_agent_string(MUST(String::from_utf8(user_agent)));
        });
        action->set_checked(options.user_agent == user_agent);
        spoof_user_agent_menu->add_action(move(action));
    };
    add_user_agent("Disabled"sv, Web::default_user_agent);
    for (auto const& user_agent : WebView::user_agents)
        add_user_agent(user_agent.key, user_agent.value);

    auto navigator_compatibility_mode_menu = Menu::create_group("Navigator Compatibility Mode"sv);
    auto add_navigator_compatibility_mode = [&](StringView name, Web::NavigatorCompatibilityMode mode) {
        auto action = Action::create_checkable(name, ActionID::SpoofUserAgent, [mode] {
            HubConnection::the().async_set_navigator_compatibility_mode(mode);
        });
        action->set_checked(options.navigator_compatibility_mode == mode);
        navigator_compatibility_mode_menu->add_action(move(action));
    };
    add_navigator_compatibility_mode("Chrome"sv, Web::NavigatorCompatibilityMode::Chrome);
    add_navigator_compatibility_mode("Gecko"sv, Web::NavigatorCompatibilityMode::Gecko);
    add_navigator_compatibility_mode("WebKit"sv, Web::NavigatorCompatibilityMode::WebKit);

    m_debug_menu->add_submenu(move(spoof_user_agent_menu));
    m_debug_menu->add_submenu(move(navigator_compatibility_mode_menu));
    m_debug_menu->add_separator();

    add_switch(m_enable_scripting_action, "Enable Scripting"sv, ActionID::EnableScripting, options.scripting_enabled, [](bool enabled) {
        HubConnection::the().async_set_scripting_enabled(enabled);
    });
    add_switch(m_block_pop_ups_action, "Block Pop-ups"sv, ActionID::BlockPopUps, options.blocks_pop_ups, [](bool blocks) {
        HubConnection::the().async_set_blocks_pop_ups(blocks);
    });
}

void ApplicationActions::update_for_active_view()
{
    auto* view = active_view();
    // NB: The page's undo state says nothing about a focused text field, where these actions go instead.
    auto text_field_has_focus = is_text_field(QApplication::focusWidget());
    m_undo_action->set_enabled(text_field_has_focus || (view && view->can_undo()));
    m_redo_action->set_enabled(text_field_has_focus || (view && view->can_redo()));
    m_view_source_action->set_enabled(!view || view->can_view_source());
    m_report_website_issue_action->set_enabled(view && view->can_report_website_issue());
    update_force_dark_action();
}

void ApplicationActions::update_force_dark_action()
{
    auto& settings = Application::settings();
    auto* view = active_view();
    m_force_dark_action->set_enabled(view && view->is_private() == WebView::IsPrivate::No && !view->current_host_for_settings().is_empty());
    m_force_dark_action->set_checked(view ? settings.force_dark_for_host(view->current_host_for_settings()) : settings.content_settings().enable_force_dark);
}

void ApplicationActions::devtools_enabled_changed(bool enabled)
{
    m_toggle_devtools_action->set_text(enabled ? "Disable DevTools"sv : "Enable DevTools"sv);
}

void ApplicationActions::update_website_color_scheme_actions()
{
    auto preference = Application::settings().appearance().website_color_scheme;
    auto index = preference == WebView::ThemePreference::System ? 0 : preference == WebView::ThemePreference::Dark ? 1
                                                                                                                   : 2;
    m_color_scheme_menu->items()[index].get<NonnullRefPtr<Action>>()->set_checked(true);
}

void ApplicationActions::update_vertical_tabs_action()
{
    auto const& tab_settings = Application::settings().tab_settings();
    m_toggle_vertical_tabs_expanded_action->set_visible(tab_settings.vertical_tabs_enabled);
    m_toggle_vertical_tabs_expanded_action->set_engaged(tab_settings.vertical_tabs_expanded);
    auto text = tab_settings.vertical_tabs_expanded ? "Hide Sidebar"sv : "Show Sidebar"sv;
    m_toggle_vertical_tabs_expanded_action->set_text(text);
    m_toggle_vertical_tabs_expanded_action->set_tooltip(text);
}

void ApplicationActions::appearance_changed()
{
    m_toggle_menu_bar_action->set_checked(Application::settings().appearance().show_menu_bar);
    update_website_color_scheme_actions();
}

void ApplicationActions::content_settings_changed()
{
    update_force_dark_action();
}

void ApplicationActions::tab_settings_changed()
{
    update_vertical_tabs_action();
}

void ApplicationActions::config_variable_changed(WebView::ConfigVariableID variable)
{
    auto& settings = Application::settings();
    if (variable == WebView::ConfigVariableID::ShowAdvancedDebugMenu)
        m_debug_menu->set_visible(settings.config_variable_as_bool(WebView::ConfigVariableID::ShowAdvancedDebugMenu));
    if (variable == WebView::ConfigVariableID::ShowTabPerformanceMonitor)
        m_show_tab_performance_monitor_action->set_checked(settings.config_variable_as_bool(WebView::ConfigVariableID::ShowTabPerformanceMonitor));
}

}
