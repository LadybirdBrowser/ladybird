/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/FindInPageWidget.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/WebContentView.h>

#include <QBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QToolButton>
#include <QWidget>

class QTimer;

namespace Ladybird {

class BrowserWindow;
class WindowControlButton;

class HyperlinkLabel final : public QLabel {
    Q_OBJECT

public:
    explicit HyperlinkLabel(QWidget* parent = nullptr)
        : QLabel(parent)
    {
        setMouseTracking(true);
    }

    virtual void enterEvent(QEnterEvent* event) override
    {
        emit mouse_entered(event);
    }

signals:
    void mouse_entered(QEnterEvent*);
};

class Tab final
    : public QWidget
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    Tab(BrowserWindow* window, RefPtr<WebView::WebContentClient> parent_client = nullptr, size_t page_index = 0);
    virtual ~Tab() override;

    WebContentView& view() { return *m_view; }
    WebContentView const& view() const { return *m_view; }

    void navigate(URL::URL const&);
    void load_html(StringView);

    void open_file();

    void show_find_in_page();
    void find_previous();
    void find_next();

    BookmarksBar& bookmarks_bar() { return *m_bookmarks_bar; }

    void request_close();

    QIcon const& favicon() const { return m_favicon; }
    QIcon tab_icon() const;
    QString title() const;

    QMenu* context_menu() const { return m_context_menu; }

    QToolButton* hamburger_button() const { return m_hamburger_button; }
    QWidget* toolbar_container() const { return m_toolbar_container; }

    void set_vertical_tabs_enabled(bool);
    void set_window(BrowserWindow&);
    void set_toolbar_container_in_tab_layout(bool);
    void set_toolbar_window_controls_visible(bool);
    void update_window_control_icons();
    void update_hover_label();

    bool url_is_hidden() const { return m_location_edit->url_is_hidden(); }
    void set_url_is_hidden(bool url_is_hidden) { m_location_edit->set_url_is_hidden(url_is_hidden); }

public slots:
    void focus_location_editor();
    void location_edit_return_pressed();

signals:
    void title_changed(int id, QString const&);
    void favicon_changed(int id, QIcon const&);
    void audio_play_state_changed(int id, Web::HTML::AudioPlayState);

private:
    virtual void resizeEvent(QResizeEvent*) override;
    virtual bool event(QEvent*) override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;

    void recreate_toolbar_icons();
    void connect_hamburger_menu();
    void update_chrome_style();
    void update_tab_title();
    void set_loading(bool);
    void update_tab_icon();
    int tab_index();

    QWidget* m_toolbar_container { nullptr };
    QWidget* m_toolbar { nullptr };
    QWidget* m_toolbar_window_controls_separator { nullptr };
    QWidget* m_toolbar_window_controls { nullptr };
    QSpacerItem* m_toolbar_window_controls_spacer { nullptr };
    QSpacerItem* m_sidebar_toggle_navigation_spacer { nullptr };
    WindowControlButton* m_minimize_window_button { nullptr };
    WindowControlButton* m_maximize_window_button { nullptr };
    WindowControlButton* m_close_window_button { nullptr };
    BookmarksBar* m_bookmarks_bar { nullptr };
    QToolButton* m_hamburger_button { nullptr };
    LocationEdit* m_location_edit { nullptr };
    WebContentView* m_view { nullptr };
    FindInPageWidget* m_find_in_page { nullptr };
    BrowserWindow* m_window { nullptr };
    QString m_title;
    HyperlinkLabel* m_hover_label { nullptr };
    QIcon m_favicon;
    QTimer* m_loading_animation_timer { nullptr };
    bool m_is_loading { false };
    bool m_is_updating_chrome_style { false };
    int m_loading_animation_frame { 0 };

    QMenu* m_context_menu { nullptr };
    QMenu* m_page_context_menu { nullptr };
    QMenu* m_link_context_menu { nullptr };
    QMenu* m_image_context_menu { nullptr };
    QMenu* m_media_context_menu { nullptr };
    QMenu* m_select_dropdown { nullptr };

    QAction* m_toggle_vertical_tabs_expanded_action { nullptr };
    QAction* m_navigate_back_action { nullptr };
    QAction* m_navigate_forward_action { nullptr };
    QAction* m_reload_action { nullptr };

    QPointer<QDialog> m_dialog;

    bool m_already_requested_close { false };
};

}
