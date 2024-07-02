/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <LibWebView/SearchEngine.h>
#include <QPoint>
#include <QSettings>
#include <QSize>

namespace Ladybird {

class Settings : public QObject {
    Q_OBJECT

public:
    Settings(Settings const&) = delete;
    Settings& operator=(Settings const&) = delete;

    static Settings* the()
    {
        static Settings instance;
        return &instance;
    }

    Optional<QPoint> last_position();
    void set_last_position(QPoint const& last_position);

    QSize last_size();
    void set_last_size(QSize const& last_size);

    bool is_maximized();
    void set_is_maximized(bool is_maximized);

    QString new_tab_page();
    void set_new_tab_page(QString const& page);

    WebView::SearchEngine search_engine() const { return m_search_engine; }
    void set_search_engine(WebView::SearchEngine engine);

    struct EngineProvider {
        QString name;
        QString url;
    };

    EngineProvider autocomplete_engine();
    void set_autocomplete_engine(EngineProvider const& engine);

    bool enable_autocomplete();
    void set_enable_autocomplete(bool enable);

    bool enable_search();
    void set_enable_search(bool enable);

    bool enable_do_not_track();
    void set_enable_do_not_track(bool enable);

    bool show_menubar();
    void set_show_menubar(bool show_menubar);

signals:
    void show_menubar_changed(bool show_menubar);
    void enable_search_changed(bool enable);
    void search_engine_changed(WebView::SearchEngine engine);
    void enable_do_not_track_changed(bool enable);

protected:
    Settings();

private:
    OwnPtr<QSettings> m_qsettings;
    WebView::SearchEngine m_search_engine;
};

}
