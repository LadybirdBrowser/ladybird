/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWebView/Forward.h>
#include <UI/Qt/Tab.h>

#include <QIcon>
#include <QMainWindow>
#include <QMenuBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>

namespace Ladybird {

class Tab;
class WebContentView;

class BrowserWindow : public QMainWindow {
    Q_OBJECT

public:
    enum class IsPopupWindow {
        No,
        Yes,
    };

    BrowserWindow(Vector<URL::URL> const& initial_urls, IsPopupWindow is_popup_window = IsPopupWindow::No, Tab* parent_tab = nullptr, Optional<u64> page_index = {});

    WebContentView& view() const { return m_current_tab->view(); }

    int tab_count() { return m_tabs_container->count(); }
    int tab_index(Tab*);

    Tab& create_new_tab(Web::HTML::ActivateTab activate_tab);
    Tab* current_tab() const { return m_current_tab; }

    QMenu& hamburger_menu() const { return *m_hamburger_menu; }

    QAction& new_tab_action() const { return *m_new_tab_action; }
    QAction& new_window_action() const { return *m_new_window_action; }
    QAction& find_action() const { return *m_find_in_page_action; }

    double refresh_rate() const { return m_refresh_rate; }

    void on_devtools_enabled();
    void on_devtools_disabled();

public slots:
    void device_pixel_ratio_changed(qreal dpi);
    void refresh_rate_changed(qreal refresh_rate);
    void tab_title_changed(int index, QString const&);
    void tab_favicon_changed(int index, QIcon const& icon);
    void tab_audio_play_state_changed(int index, Web::HTML::AudioPlayState);
    Tab& new_tab_from_url(URL::URL const&, Web::HTML::ActivateTab);
    Tab& new_child_tab(Web::HTML::ActivateTab, Tab& parent, Optional<u64> page_index);
    void activate_tab(int index);
    void close_tab(int index);
    void move_tab(int old_index, int new_index);
    void close_current_tab();
    void open_next_tab();
    void open_previous_tab();
    void open_file();
    void show_find_in_page();

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

    void set_current_tab(Tab* tab) { m_current_tab = tab; }

    template<typename Callback>
    void for_each_tab(Callback&& callback)
    {
        for (int i = 0; i < m_tabs_container->count(); ++i) {
            auto& tab = as<Tab>(*m_tabs_container->widget(i));
            callback(tab);
        }
    }

    void create_close_button_for_tab(Tab*);

    QIcon icon_for_page_mute_state(Tab&) const;
    QString tool_tip_for_page_mute_state(Tab&) const;
    QTabBar::ButtonPosition audio_button_position_for_tab(int tab_index) const;

    void set_window_rect(Optional<Web::DevicePixels> x, Optional<Web::DevicePixels> y, Optional<Web::DevicePixels> width, Optional<Web::DevicePixels> height);

    QScreen* m_current_screen { nullptr };
    double m_device_pixel_ratio { 0 };
    double m_refresh_rate { 60.0 };

    QTabWidget* m_tabs_container { nullptr };
    Tab* m_current_tab { nullptr };

    QToolBar* m_new_tab_button_toolbar { nullptr };

    QMenu* m_hamburger_menu { nullptr };

    QAction* m_new_tab_action { nullptr };
    QAction* m_new_window_action { nullptr };
    QAction* m_find_in_page_action { nullptr };

    IsPopupWindow m_is_popup_window { IsPopupWindow::No };
};

}
