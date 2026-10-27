/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <UI/Qt/ChromeStyle.h>

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QSize>

class QPainter;
class QPalette;
class QRectF;

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
    Volume,
    VolumeMuted,
    ChevronUp,
    ChevronDown,
    ContentBlocking,
    SiteSettings,
    Microphone,
    Location,
    SelectArea,
    CheckCircle,
    Document,
    Network,
    Compositor,
    Image,
    Pin,
    Warning,
    Pause,
    ZoomOut,
    Sidebar,
    SidebarRight,
    WindowMinimize,
    WindowMaximize,
    WindowRestore,
    WindowClose,
};

constexpr inline auto ICON_DEVICE_PIXEL_RATIOS = to_array({ 1, 2, 3 });
// Chrome icons are drawn on a canvas of this size.
constexpr inline int CHROME_ICON_CANVAS_SIZE = 16;
// The toolbar's buttons show their icons larger than the controls inside the address field.
constexpr inline int TOOLBAR_ICON_SIZE = 18;

QIcon load_icon_from_uri(StringView);
QIcon icon_from_png(ReadonlyBytes, int logical_size);
QPixmap favicon_with_rounded_corners(QPixmap const&);
QIcon favicon_with_rounded_corners(QIcon const&, int logical_size);
QIcon create_chrome_icon(ChromeIcon, QPalette const&, ChromeStyle::WindowActivity = ChromeStyle::WindowActivity::Active, int logical_size = CHROME_ICON_CANVAS_SIZE);
// The site settings icon with its two knobs moved toward each other's place, by a travel from 0 to 1.
QIcon create_site_settings_icon(QPalette const&, qreal knob_travel);
// The icon in one color, whatever its state.
QIcon create_chrome_icon(ChromeIcon, QColor const&);
QPixmap create_chrome_icon_pixmap(ChromeIcon, QColor, qreal device_pixel_ratio);

QSize physical_size_for_device_pixel_ratio(QSize size, qreal device_pixel_ratio);

// Strokes Ladybird's logomark, a Lissajous figure, to fill the given rect. The stroke width is on the logomark's own
// 22 by 18.4 box.
void paint_logomark(QPainter&, QRectF const&, QColor const&, qreal stroke_width);

}
