/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/AudioPlayState.h>
#include <UI/Qt/FindInPageWidget.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/WebContentView.h>

#include <QBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

namespace Ladybird {

class BrowserWindow;

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

class Tab final : public QWidget {
    Q_OBJECT

public:
    Tab(BrowserWindow* window, RefPtr<WebView::WebContentClient> const& parent_client = nullptr, size_t page_index = 0);
    virtual ~Tab() override;

    WebContentView& view() { return *m_view; }

    void navigate(URL::URL const&);
    void load_html(StringView);

    void open_file();

    void show_find_in_page();
    void find_previous();
    void find_next();

    QIcon const& favicon() const { return m_favicon; }
    QString const& title() const { return m_title; }

    QMenu* context_menu() const { return m_context_menu; }

    QToolButton* hamburger_button() const { return m_hamburger_button; }

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

    void recreate_toolbar_icons();
    int tab_index();

    QBoxLayout* m_layout { nullptr };
    QToolBar* m_toolbar { nullptr };
    QToolButton* m_hamburger_button { nullptr };
    QAction* m_hamburger_button_action { nullptr };
    LocationEdit* m_location_edit { nullptr };
    WebContentView* m_view { nullptr };
    FindInPageWidget* m_find_in_page { nullptr };
    BrowserWindow* m_window { nullptr };
    QString m_title;
    HyperlinkLabel* m_hover_label { nullptr };
    QIcon m_favicon;

    QMenu* m_context_menu { nullptr };
    QMenu* m_page_context_menu { nullptr };
    QMenu* m_link_context_menu { nullptr };
    QMenu* m_image_context_menu { nullptr };
    QMenu* m_media_context_menu { nullptr };
    QMenu* m_select_dropdown { nullptr };

    QAction* m_navigate_back_action { nullptr };
    QAction* m_navigate_forward_action { nullptr };
    QAction* m_reload_action { nullptr };

    QPointer<QDialog> m_dialog;
};

}
