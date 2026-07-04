/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>

#include <QIcon>
#include <QMainWindow>
#include <QPushButton>
#include <QRect>
#include <QTabBar>

class QPropertyAnimation;
class QWindow;
class QToolButton;
class QWidget;

namespace Ladybird {

class Tab;
class WebContentView;
class BrowserWindow;
class DevToolsBanner;

class ExitFullscreenButton : public QPushButton {
    Q_OBJECT

public:
    ExitFullscreenButton(QWidget* parent = nullptr);
    ~ExitFullscreenButton() override = default;
    void animate_show();

private:
    QPropertyAnimation* m_widget_animation;
};

// Handles Qt UI state related to when Ladybird has entered fullscreen,
// like displaying an exit button, listening for escape key presses and so on
class FullscreenMode : public QObject {
    Q_OBJECT

public:
    static constexpr int button_animation_time() { return 750; }
    explicit FullscreenMode(BrowserWindow* window, ExitFullscreenButton* exit_button);

    enum class ExitInitiatedBy {
        UI,
        WebContent,
    };

    void exit(ExitInitiatedBy);
    void enter(Tab* tab);
    // Called after a window change event that has identifed the current window state to be fullscreen.
    void entered_fullscreen();
    bool is_api_fullscreen() const;
signals:
    void on_exit_fullscreen();

protected:
    // FullscreenMode's eventFilter is responsible for things that need to happen when a document
    // is in "fullscreen API fullscreen", like exiting when pushing escape, showing the exit button
    virtual bool eventFilter(QObject* obj, QEvent* event) override;

private:
    bool debounce() const;
    // Called when in fullscreen. Displays exit fullscreen button if mouse comes close to the top of the screen.
    void maybe_animate_show_exit_button(QPointF pos);
    BrowserWindow* m_window;
    ExitFullscreenButton* m_exit_button;
    // Never access this directly. First check m_window->tab_index(m_fullscreen_tab) != -1, to verify it's liveness.
    Tab* m_fullscreen_tab { nullptr };
    bool m_debounce { false };
};

class BrowserWindow
    : public QMainWindow
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    enum class IsPopupWindow {
        No,
        Yes,
    };

    BrowserWindow(Vector<URL::URL> const& initial_urls, IsPopupWindow is_popup_window = IsPopupWindow::No, Tab* parent_tab = nullptr, Optional<u64> page_index = {});
    virtual ~BrowserWindow() override;

    WebContentView& view() const { return m_current_tab->view(); }

    int tab_count() { return m_tabs_container->count(); }
    int tab_index(Tab*);

    Tab& create_new_tab(Web::HTML::ActivateTab activate_tab);
    Tab* current_tab() const { return m_current_tab; }
    bool activate_tab_with_url(URL::URL const&);
    FullscreenMode& fullscreen_mode();

    QMenu& hamburger_menu() const { return *m_hamburger_menu; }
    static bool uses_client_side_decorations();

    QAction& new_window_action() const { return *m_new_window_action; }
    QAction& find_action() const { return *m_find_in_page_action; }

    void update_tabs_display();

    void rebuild_bookmarks_menu();
    void update_reopen_recently_closed_action();
    void detach_tab_to_new_window(int index, QPoint global_position);
    void move_tab_to_window(int index, BrowserWindow& target_window, int target_index);
    void adopt_tab(Tab&, int index);

    double refresh_rate() const { return m_refresh_rate; }
    Optional<u64> display_id() const { return m_display_id; }

    void on_devtools_enabled();
    void on_devtools_disabled();

    void set_window_rect(Optional<Web::DevicePixels> x, Optional<Web::DevicePixels> y, Optional<Web::DevicePixels> width, Optional<Web::DevicePixels> height);

public slots:
    void device_pixel_ratio_changed(qreal dpi);
    void refresh_rate_changed(qreal refresh_rate);
    void tab_title_changed(int index, QString const&);
    void tab_favicon_changed(int index, QIcon const& icon);
    void tab_audio_play_state_changed(int index, Web::HTML::AudioPlayState);
    Tab& new_tab_from_url(URL::URL const&, Web::HTML::ActivateTab);
    Tab& new_child_tab(Web::HTML::ActivateTab, Tab& parent, Optional<u64> page_index);
    void activate_tab(int index);
    bool definitely_close_tab(int index);
    void move_tab(int old_index, int new_index);
    void request_to_close_tab(int index);
    void request_to_close_current_tab();
    void open_next_tab();
    void open_previous_tab();
    void open_file();
    void show_find_in_page();
    void enter_fullscreen();
    void exit_fullscreen();

private:
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void changeEvent(QEvent* event) override;
    virtual void moveEvent(QMoveEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;
    virtual void closeEvent(QCloseEvent*) override;

    virtual void show_menu_bar_changed() override;
    virtual void show_bookmarks_bar_changed() override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;

    Tab& create_new_tab(Web::HTML::ActivateTab, Tab& parent, Optional<u64> page_index);
    void initialize_tab(Tab*);
    void uninitialize_tab(Tab*);

    void set_current_tab(Tab* tab);
    bool position_is_in_rounded_corner_cutout(QPoint const&) const;
    Qt::Edges resize_edges_for_position(QPoint const&) const;
    Optional<Qt::CursorShape> resize_cursor_for_edges(Qt::Edges) const;
    bool filter_native_window_event(QWindow&, QEvent&);
    bool start_window_resize(Qt::Edges, QPoint const& global_position);
    void update_window_resize(QPoint const& global_position);
    void finish_window_resize();
    void update_resize_cursor(QPoint const&);
    void refresh_resize_cursor_at_current_position(bool force_reapply = false);
    void clear_resize_cursor();
    void update_window_corners();
    void update_appkit_window_resizability();
    bool should_draw_window_border() const;
    void update_window_border();

    template<typename Callback>
    void for_each_tab(Callback&& callback)
    {
        for (int i = 0; i < m_tabs_container->count(); ++i)
            callback(*m_tabs_container->tab(i));
    }

    void initialize_tab_buttons(Tab*);
    void create_menu_bar_window_controls();
    void update_tab_button_icons();
    void update_menu_bar_style();
    void update_menu_bar_visibility();
    void update_menu_bar_window_control_icons();
    void update_window_decoration_state();
    void toggle_window_maximized();
    bool start_window_move();
    bool connect_window_screen_changed_signal();
    void disconnect_window_screen_changed_signal();
    void connect_screen_signals(QScreen*);
    void disconnect_screen_signals(QScreen*);
    void screen_changed(QScreen*);
    void display_metadata_changed(Optional<u64> display_id, qreal refresh_rate);

    QIcon icon_for_page_mute_state(Tab&) const;
    QString tool_tip_for_page_mute_state(Tab&) const;

    QScreen* m_current_screen { nullptr };
    QWindow* m_window_screen_changed_signal_window { nullptr };
    Optional<u64> m_display_id;
    double m_device_pixel_ratio { 0 };
    double m_refresh_rate { 60.0 };

    TabWidget* m_tabs_container { nullptr };
    Tab* m_current_tab { nullptr };
    DevToolsBanner* m_devtools_banner { nullptr };

    QMenu* m_hamburger_menu { nullptr };
    QMenu* m_bookmarks_menu { nullptr };
    QMenu* m_history_menu { nullptr };
    QWidget* m_menu_bar_window_controls { nullptr };
    QToolButton* m_menu_bar_minimize_window_button { nullptr };
    QToolButton* m_menu_bar_maximize_window_button { nullptr };
    QToolButton* m_menu_bar_close_window_button { nullptr };

    QAction* m_new_tab_action { nullptr };
    QAction* m_new_window_action { nullptr };
    QAction* m_reopen_recently_closed_tab_action { nullptr };
    QAction* m_find_in_page_action { nullptr };

    IsPopupWindow m_is_popup_window { IsPopupWindow::No };

    ExitFullscreenButton* m_exit_button { nullptr };
    FullscreenMode* m_fullscreen_mode { nullptr };
    // Determine if window should restore to maximized or normal, when exiting fullscreen.
    bool m_restore_to_maximized { false };
    bool m_should_record_closed_window_on_close { true };
    bool m_resize_cursor_active { false };
    bool m_is_resizing_window { false };
    Qt::Edges m_resize_edges {};
    QPoint m_resize_start_global_position;
    QRect m_resize_start_geometry;
};

}
