/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StringView.h>

#include <QIcon>
#include <QSize>

class QPalette;

namespace Ladybird {

enum class ChromeIcon {
    Back,
    Forward,
    Reload,
    Stop,
    NewTab,
    Close,
    TabClose,
    Menu,
    Star,
    StarFilled,
    Search,
    Globe,
    Folder,
    Volume,
    VolumeMuted,
    ChevronUp,
    ChevronDown,
    VerticalTabBarCollapse,
    VerticalTabBarExpand,
    WindowMinimize,
    WindowMaximize,
    WindowRestore,
    WindowClose,
};

constexpr inline auto ICON_DEVICE_PIXEL_RATIOS = to_array({ 1, 2, 3 });

QIcon load_icon_from_uri(StringView);
QIcon icon_from_base64_png(StringView, int logical_size);
QIcon create_chrome_icon(ChromeIcon, QPalette const&);
QIcon loading_spinner_icon(QPalette const& palette, int frame);

QSize physical_size_for_device_pixel_ratio(QSize size, qreal device_pixel_ratio);

}
