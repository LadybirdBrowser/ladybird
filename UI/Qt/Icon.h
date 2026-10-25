/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Span.h>
#include <AK/StringView.h>

#include <QColor>
#include <QIcon>
#include <QPixmap>
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
    Download,
    DownloadActive,
    Volume,
    VolumeMuted,
    ChevronUp,
    ChevronDown,
    ContentBlocking,
    VerticalTabBarCollapse,
    VerticalTabBarCollapseRight,
    VerticalTabBarExpand,
    VerticalTabBarExpandRight,
    WindowMinimize,
    WindowMaximize,
    WindowRestore,
    WindowClose,
};

constexpr inline auto ICON_DEVICE_PIXEL_RATIOS = to_array({ 1, 2, 3 });
// Chrome icons are drawn on a canvas of this size, with room around their shapes.
constexpr inline int CHROME_ICON_CANVAS_SIZE = 20;

QIcon load_icon_from_uri(StringView);
QIcon icon_from_png(ReadonlyBytes, int logical_size);
QPixmap favicon_with_rounded_corners(QPixmap const&);
QIcon favicon_with_rounded_corners(QIcon const&, int logical_size);
QIcon create_chrome_icon(ChromeIcon, QPalette const&);
QPixmap create_chrome_icon_pixmap(ChromeIcon, QColor, qreal device_pixel_ratio);
QIcon loading_spinner_icon(QPalette const& palette, int frame);

QSize physical_size_for_device_pixel_ratio(QSize size, qreal device_pixel_ratio);

}
