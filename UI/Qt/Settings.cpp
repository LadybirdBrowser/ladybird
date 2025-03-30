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
