/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Settings.h"
#include "StringUtils.h"
#include <Ladybird/DefaultSettings.h>

namespace Ladybird {

Settings::Settings()
    : m_search_engine(WebView::default_search_engine())
{
    m_qsettings = make<QSettings>("Ladybird", "Ladybird", this);

    auto default_search_engine = WebView::default_search_engine();
    auto default_search_engine_name = qstring_from_ak_string(default_search_engine.name);

    auto search_engine_name = m_qsettings->value("search_engine_name", default_search_engine_name).toString();
    auto search_engine = WebView::find_search_engine_by_name(ak_string_from_qstring(search_engine_name));

    if (search_engine.has_value())
        m_search_engine = search_engine.release_value();
    else
        set_search_engine(move(default_search_engine));
}

Optional<QPoint> Settings::last_position()
{
    if (m_qsettings->contains("last_position"))
        return m_qsettings->value("last_position", QPoint()).toPoint();
    return {};
}

void Settings::set_last_position(QPoint const& last_position)
{
    m_qsettings->setValue("last_position", last_position);
}

QSize Settings::last_size()
{
    return m_qsettings->value("last_size", QSize(800, 600)).toSize();
}

void Settings::set_last_size(QSize const& last_size)
{
    m_qsettings->setValue("last_size", last_size);
}

bool Settings::is_maximized()
{
    return m_qsettings->value("is_maximized", QVariant(false)).toBool();
}

void Settings::set_is_maximized(bool is_maximized)
{
    m_qsettings->setValue("is_maximized", is_maximized);
}

void Settings::set_search_engine(WebView::SearchEngine search_engine)
{
    m_qsettings->setValue("search_engine_name", qstring_from_ak_string(search_engine.name));
    m_search_engine = move(search_engine);
    emit search_engine_changed(m_search_engine);
}

Settings::EngineProvider Settings::autocomplete_engine()
{
    EngineProvider engine_provider;
    engine_provider.name = m_qsettings->value("autocomplete_engine_name", "Google").toString();
    engine_provider.url = m_qsettings->value("autocomplete_engine", "https://www.google.com/complete/search?client=chrome&q={}").toString();
    return engine_provider;
}

void Settings::set_autocomplete_engine(EngineProvider const& engine_provider)
{
    m_qsettings->setValue("autocomplete_engine_name", engine_provider.name);
    m_qsettings->setValue("autocomplete_engine", engine_provider.url);
}

QString Settings::new_tab_page()
{
    static auto const default_new_tab_url = qstring_from_ak_string(Browser::default_new_tab_url);
    return m_qsettings->value("new_tab_page", default_new_tab_url).toString();
}

void Settings::set_new_tab_page(QString const& page)
{
    m_qsettings->setValue("new_tab_page", page);
}

bool Settings::enable_autocomplete()
{
    return m_qsettings->value("enable_autocomplete", false).toBool();
}

void Settings::set_enable_autocomplete(bool enable)
{
    m_qsettings->setValue("enable_autocomplete", enable);
}

bool Settings::enable_search()
{
    return m_qsettings->value("enable_search", false).toBool();
}

void Settings::set_enable_search(bool enable)
{
    m_qsettings->setValue("enable_search", enable);
    emit enable_search_changed(enable);
}

bool Settings::enable_do_not_track()
{
    return m_qsettings->value("enable_do_not_track", false).toBool();
}

void Settings::set_enable_do_not_track(bool enable)
{
    m_qsettings->setValue("enable_do_not_track", enable);
    emit enable_do_not_track_changed(enable);
}

bool Settings::show_menubar()
{
    return m_qsettings->value("show_menubar", false).toBool();
}

void Settings::set_show_menubar(bool show_menubar)
{
    m_qsettings->setValue("show_menubar", show_menubar);
    emit show_menubar_changed(show_menubar);
}

}
