/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

namespace Ladybird {

Settings::Settings()
{
    m_qsettings = make<QSettings>(QSettings::IniFormat, QSettings::UserScope, "Ladybird", "Ladybird", this);
}

ByteString Settings::directory()
{
    return LexicalPath::dirname(ak_byte_string_from_qstring(m_qsettings->fileName()));
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

QStringList Settings::preferred_languages()
{
    return m_qsettings->value("preferred_languages").toStringList();
}

void Settings::set_preferred_languages(QStringList const& languages)
{
    m_qsettings->setValue("preferred_languages", languages);
    emit preferred_languages_changed(languages);
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

bool Settings::enable_autocomplete()
{
    return m_qsettings->value("enable_autocomplete", false).toBool();
}

void Settings::set_enable_autocomplete(bool enable)
{
    m_qsettings->setValue("enable_autocomplete", enable);
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
