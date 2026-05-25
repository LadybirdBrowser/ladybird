/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

#include <QIcon>

class QPalette;

namespace Ladybird {

enum class ChromeIcon {
    Back,
    Forward,
    Reload,
    Stop,
    NewTab,
    Close,
    Menu,
    Star,
    StarFilled,
    Search,
    Globe,
    WindowMinimize,
    WindowMaximize,
    WindowRestore,
    WindowClose,
};

QIcon load_icon_from_uri(StringView);
QIcon create_tvg_icon_with_theme_colors(QString const& name, QPalette const& palette);
QIcon create_chrome_icon(ChromeIcon, QPalette const&);
QIcon loading_spinner_icon(QPalette const& palette, int frame);

}
