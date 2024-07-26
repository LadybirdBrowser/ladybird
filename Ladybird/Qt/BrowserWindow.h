/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Tab.h"
#include <Ladybird/Qt/FindInPageWidget.h>
#include <Ladybird/Types.h>
#include <LibCore/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWebView/Forward.h>
#include <QIcon>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QTabWidget>
#include <QToolBar>

namespace Ladybird {

class SettingsDialog;
class WebContentView;
class TaskManagerWindow;

class BrowserWindow : public QMainWindow {
    Q_OBJECT

public:
    BrowserWindow(Vector<URL::URL> const& initial_urls, WebView::CookieJar&, WebContentOptions const&, StringView webdriver_content_ipc_path, bool allow_popups, Tab* parent_tab = nullptr, Optional<u64> page_index = {});

    WebContentView& view() const { return m_current_tab->view(); }

    int tab_count() { return m_tabs_container->count(); }
    int tab_index(Tab*);
    Tab& create_new_tab(Web::HTML::ActivateTab activate_tab);

    QMenu& hamburger_menu()
    {
        return *m_hamburger_menu;
    }

    QAction& go_back_action()
    {
        return *m_go_back_action;
    }

    QAction& go_forward_action()
    {
        return *m_go_forward_action;
    }

    QAction& reload_action()
    {
        return *m_reload_action;
    }

    QAction& new_tab_action()
    {
        return *m_new_tab_action;
    }

    QAction& new_window_action()
    {
        return *m_new_window_action;
    }

    QAction& copy_selection_action()
    {
        return *m_copy_selection_action;
    }

    QAction& select_all_action()
    {
        return *m_select_all_action;
    }

    QAction& find_action()
    {
        return *m_find_in_page_action;
    }

    QAction& paste_action()
    {
        return *m_paste_action;
    }

    QAction& view_source_action()
    {
        return *m_view_source_action;
    }

    QAction& inspect_dom_node_action()
    {
        return *m_inspect_dom_node_action;
    }

    Tab* current_tab() const { return m_current_tab; }

public slots:
    void device_pixel_ratio_changed(qreal dpi);
    void tab_title_changed(int index, QString const&);
    void tab_favicon_changed(int index, QIcon const& icon);
    void tab_audio_play_state_changed(int index, Web::HTML::AudioPlayState);
    void tab_navigation_buttons_state_changed(int index);
    Tab& new_tab_from_url(URL::URL const&, Web::HTML::ActivateTab);
    Tab& new_tab_from_content(StringView html, Web::HTML::ActivateTab);
    Tab& new_child_tab(Web::HTML::ActivateTab, Tab& parent, Optional<u64> page_index);
    void activate_tab(int index);
    void close_tab(int index);
    void move_tab(int old_index, int new_index);
    void close_current_tab();
    void open_next_tab();
    void open_previous_tab();
    void open_file();
    void enable_auto_contrast();
    void enable_less_contrast();
    void enable_more_contrast();
    void enable_no_preference_contrast();
    void enable_auto_motion();
    void enable_no_preference_motion();
    void enable_reduce_motion();
    void zoom_in();
    void zoom_out();
    void reset_zoom();
    void update_zoom_menu();
    void select_all();
    void show_find_in_page();
    void paste();
    void copy_selected_text();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    virtual bool event(QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void moveEvent(QMoveEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;
    virtual void closeEvent(QCloseEvent*) override;

    Tab& create_new_tab(Web::HTML::ActivateTab, Tab& parent, Optional<u64> page_index);
    void initialize_tab(Tab*);

    void debug_request(ByteString const& request, ByteString const& argument = "");

    void set_current_tab(Tab* tab);
    void update_displayed_zoom_level();

    template<typename Callback>
    void for_each_tab(Callback&& callback)
    {
        for (int i = 0; i < m_tabs_container->count(); ++i) {
            auto& tab = verify_cast<Tab>(*m_tabs_container->widget(i));
            callback(tab);
        }
    }

    void create_close_button_for_tab(Tab*);

    QIcon icon_for_page_mute_state(Tab&) const;
    QString tool_tip_for_page_mute_state(Tab&) const;
    QTabBar::ButtonPosition audio_button_position_for_tab(int tab_index) const;

    void set_window_rect(Optional<Web::DevicePixels> x, Optional<Web::DevicePixels> y, Optional<Web::DevicePixels> width, Optional<Web::DevicePixels> height);

    ByteString user_agent_string() const { return m_user_agent_string; }
    void set_user_agent_string(ByteString const& user_agent_string) { m_user_agent_string = user_agent_string; }
    ByteString navigator_compatibility_mode() const { return m_navigator_compatibility_mode; }
    void set_navigator_compatibility_mode(ByteString const& navigator_compatibility_mode) { m_navigator_compatibility_mode = navigator_compatibility_mode; }

    QScreen* m_current_screen;
    double m_device_pixel_ratio { 0 };

    Web::CSS::PreferredColorScheme m_preferred_color_scheme;
    void set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme);

    QTabWidget* m_tabs_container { nullptr };
    Tab* m_current_tab { nullptr };
    QMenu* m_zoom_menu { nullptr };

    QToolBar* m_new_tab_button_toolbar { nullptr };

    QMenu* m_hamburger_menu { nullptr };

    QAction* m_go_back_action { nullptr };
    QAction* m_go_forward_action { nullptr };
    QAction* m_reload_action { nullptr };
    QAction* m_new_tab_action { nullptr };
    QAction* m_new_window_action { nullptr };
    QAction* m_copy_selection_action { nullptr };
    QAction* m_paste_action { nullptr };
    QAction* m_select_all_action { nullptr };
    QAction* m_find_in_page_action { nullptr };
    QAction* m_view_source_action { nullptr };
    QAction* m_inspect_dom_node_action { nullptr };
    QAction* m_show_line_box_borders_action { nullptr };
    QAction* m_enable_scripting_action { nullptr };
    QAction* m_block_pop_ups_action { nullptr };
    QAction* m_enable_same_origin_policy_action { nullptr };

    ByteString m_user_agent_string {};
    ByteString m_navigator_compatibility_mode {};

    SettingsDialog* m_settings_dialog { nullptr };

    WebView::CookieJar& m_cookie_jar;

    WebContentOptions m_web_content_options;
    StringView m_webdriver_content_ipc_path;

    bool m_allow_popups { false };
};

}
