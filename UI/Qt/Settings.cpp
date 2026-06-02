/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <UI/Qt/ChromeLayout.h>
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

bool Settings::show_menubar()
{
    if (!show_menubar_option_available())
        return false;

    return m_qsettings->value("show_menubar", false).toBool();
}

void Settings::set_show_menubar(bool show_menubar)
{
    if (!show_menubar_option_available())
        show_menubar = false;
    else
        m_qsettings->setValue("show_menubar", show_menubar);

    emit show_menubar_changed(show_menubar);
}

Optional<int> Settings::vertical_tabs_expanded_width()
{
    if (m_qsettings->contains("vertical_tabs_expanded_width"))
        return m_qsettings->value("vertical_tabs_expanded_width").toInt();
    return {};
}

void Settings::set_vertical_tabs_expanded_width(int width)
{
    m_qsettings->setValue("vertical_tabs_expanded_width", width);
}

}
