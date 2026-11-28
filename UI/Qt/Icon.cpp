/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>
#include <LibCore/Resource.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <QtMath>

namespace Ladybird {

QIcon load_icon_from_uri(StringView uri)
{
    auto resource = MUST(Core::Resource::load_from_uri(uri));
    auto path = qstring_from_ak_string(resource->filesystem_path());

    return QIcon { path };
}

// Favicons get slightly rounded corners, so that square ones sit well in the round shapes of the chrome around them.
static constexpr qreal FAVICON_CORNER_RADIUS_FRACTION = 3.0 / 16.0;

QPixmap favicon_with_rounded_corners(QPixmap const& favicon)
{
    auto image = favicon.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1);

    QImage rounded(image.size(), QImage::Format_ARGB32_Premultiplied);
    rounded.fill(Qt::transparent);
    {
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QBrush(image));
        auto radius = image.width() * FAVICON_CORNER_RADIUS_FRACTION;
        painter.drawRoundedRect(QRectF(image.rect()), radius, radius);
    }
    rounded.setDevicePixelRatio(favicon.devicePixelRatio());
    return QPixmap::fromImage(rounded);
}

QIcon favicon_from_png(ReadonlyBytes favicon_png, int logical_size)
{
    QPixmap pixmap;
    if (!pixmap.loadFromData(favicon_png.data(), static_cast<uint>(favicon_png.size()), "PNG"))
        return {};

    QIcon icon;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS) {
        auto size = logical_size * device_pixel_ratio;
        auto scaled_pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled_pixmap.setDevicePixelRatio(device_pixel_ratio);
        icon.addPixmap(favicon_with_rounded_corners(scaled_pixmap));
    }
    return icon;
}

// Parses the path data of an SVG <path> element. Only the commands used by the icons below are understood: moveto,
// lineto, horizontal and vertical lineto, cubic and quadratic Bézier curves and their smooth forms, elliptical arcs and
// closepath, absolute and relative.
static QPainterPath parse_svg_path(StringView data)
{
    QPainterPath path;
    size_t index = 0;
    char command = 0;
    QPointF current;
    QPointF subpath_start;
    // The second control point of the last cubic curve, which a smooth cubic curve after it reflects.
    Optional<QPointF> last_control;
    // The control point of the last quadratic curve, which a smooth quadratic curve after it reflects.
    Optional<QPointF> last_quadratic_control;

    auto skip_separators = [&] {
        while (index < data.length() && (data[index] == ' ' || data[index] == ','))
            ++index;
    };
    auto at_number = [&] {
        skip_separators();
        if (index >= data.length())
            return false;
        auto c = data[index];
        return c == '-' || c == '.' || (c >= '0' && c <= '9');
    };
    auto read_number = [&] {
        skip_separators();
        auto start = index;
        if (data[index] == '-')
            ++index;
        auto seen_dot = false;
        while (index < data.length()) {
            auto c = data[index];
            if (c == '.') {
                // A second dot starts the next number, as in "1.5.5".
                if (seen_dot)
                    break;
                seen_dot = true;
            } else if (c < '0' || c > '9') {
                break;
            }
            ++index;
        }
        return data.substring_view(start, index - start).to_number<double>().value();
    };
    auto read_flag = [&] {
        skip_separators();
        return data[index++] == '1';
    };

    // https://www.w3.org/TR/SVG2/implnote.html#ArcConversionEndpointToCenter
    auto arc_to = [&](double rx, double ry, bool large_arc, bool sweep, QPointF end) {
        auto x1p = (current.x() - end.x()) / 2;
        auto y1p = (current.y() - end.y()) / 2;
        auto lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
        if (lambda > 1) {
            rx *= sqrt(lambda);
            ry *= sqrt(lambda);
        }
        auto numerator = (rx * rx * ry * ry) - (rx * rx * y1p * y1p) - (ry * ry * x1p * x1p);
        auto denominator = (rx * rx * y1p * y1p) + (ry * ry * x1p * x1p);
        auto coefficient = sqrt(max(0.0, numerator / denominator)) * (large_arc == sweep ? -1 : 1);
        auto cxp = coefficient * rx * y1p / ry;
        auto cyp = coefficient * -ry * x1p / rx;
        auto cx = cxp + (current.x() + end.x()) / 2;
        auto cy = cyp + (current.y() + end.y()) / 2;

        auto angle = [](double ux, double uy, double vx, double vy) {
            return AK::atan2((ux * vy) - (uy * vx), (ux * vx) + (uy * vy));
        };
        auto start_angle = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        auto sweep_angle = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!sweep && sweep_angle > 0)
            sweep_angle -= 2 * M_PI;
        else if (sweep && sweep_angle < 0)
            sweep_angle += 2 * M_PI;

        // NB: Qt measures angles counterclockwise on screen, where y grows downward, so SVG's angles are negated.
        path.arcTo(QRectF(cx - rx, cy - ry, 2 * rx, 2 * ry), -qRadiansToDegrees(start_angle), -qRadiansToDegrees(sweep_angle));
        current = end;
    };

    while (true) {
        skip_separators();
        if (index >= data.length())
            break;
        if (!at_number())
            command = data[index++];

        auto relative = command >= 'a';
        auto origin = relative ? current : QPointF();
        auto previous_control = exchange(last_control, {});
        auto previous_quadratic_control = exchange(last_quadratic_control, {});
        switch (command) {
        case 'M':
        case 'm': {
            auto x = read_number();
            auto y = read_number();
            current = origin + QPointF(x, y);
            path.moveTo(current);
            subpath_start = current;
            // Further coordinate pairs after a moveto are linetos.
            command = relative ? 'l' : 'L';
            break;
        }
        case 'L':
        case 'l': {
            auto x = read_number();
            auto y = read_number();
            current = origin + QPointF(x, y);
            path.lineTo(current);
            break;
        }
        case 'H':
        case 'h':
            current.setX((relative ? current.x() : 0) + read_number());
            path.lineTo(current);
            break;
        case 'V':
        case 'v':
            current.setY((relative ? current.y() : 0) + read_number());
            path.lineTo(current);
            break;
        case 'C':
        case 'c': {
            QPointF control1 { read_number(), read_number() };
            QPointF control2 { read_number(), read_number() };
            QPointF end { read_number(), read_number() };
            path.cubicTo(origin + control1, origin + control2, origin + end);
            last_control = origin + control2;
            current = origin + end;
            break;
        }
        case 'S':
        case 's': {
            auto control1 = previous_control.has_value() ? (2 * current) - *previous_control : current;
            QPointF control2 { read_number(), read_number() };
            QPointF end { read_number(), read_number() };
            path.cubicTo(control1, origin + control2, origin + end);
            last_control = origin + control2;
            current = origin + end;
            break;
        }
        case 'Q':
        case 'q': {
            QPointF control { read_number(), read_number() };
            QPointF end { read_number(), read_number() };
            path.quadTo(origin + control, origin + end);
            last_quadratic_control = origin + control;
            current = origin + end;
            break;
        }
        case 'T':
        case 't': {
            auto control = previous_quadratic_control.has_value() ? (2 * current) - *previous_quadratic_control : current;
            QPointF end { read_number(), read_number() };
            path.quadTo(control, origin + end);
            last_quadratic_control = control;
            current = origin + end;
            break;
        }
        case 'A':
        case 'a': {
            auto rx = read_number();
            auto ry = read_number();
            (void)read_number(); // NB: None of our icons rotate their ellipses.
            auto large_arc = read_flag();
            auto sweep = read_flag();
            QPointF end { read_number(), read_number() };
            arc_to(rx, ry, large_arc, sweep, origin + end);
            break;
        }
        case 'Z':
        case 'z':
            path.closeSubpath();
            current = subpath_start;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    return path;
}

// The icon set of the chrome, drawn on a 16 px grid. Straight lines sit on half pixels, where a one pixel line fills
// whole pixels, and 45 degree lines run through the corners of pixels, so that a 1x screen needs no fitting at all. A
// shape with a line through its middle is centered on 7.5 for the same reason. Lines keep a pixel between them.
static constexpr auto BACK_ICON_PATH = "M10.5 2.5l-5 5 5 5"sv;
static constexpr auto FORWARD_ICON_PATH = "M5.5 2.5l5 5-5 5"sv;
static constexpr auto RELOAD_ICON_PATH = "M13.41 9a5.5 5.5 0 0 1-5.41 4.5a5.5 5.5 0 1 1 3.89-9.39L13.5 5.5M13.5 2v3.5H10"sv;
static constexpr auto SITE_SETTINGS_ICON_PATH = "M1.5 4.5h7M12.5 4.5h1M1.5 11.5h1M6.5 11.5h7M12.5 4.5a2 2 0 1 1-4 0a2 2 0 1 1 4 0M6.5 11.5a2 2 0 1 1-4 0a2 2 0 1 1 4 0"sv;
static constexpr auto MICROPHONE_ICON_PATH = "M7.5 1.5a2 2 0 0 1 2 2v4a2 2 0 0 1-4 0v-4a2 2 0 0 1 2-2zM2.5 7.5a5 5 0 0 0 10 0M7.5 12.5v2M5.5 14.5h4"sv;
static constexpr auto LOCATION_ICON_PATH = "M7.5 14.5C4.5 11.3 2.5 8.8 2.5 6.5a5 5 0 0 1 10 0c0 2.3-2 4.8-5 8zM9.5 6.5a2 2 0 1 1-4 0a2 2 0 1 1 4 0"sv;
static constexpr auto NOTIFICATIONS_ICON_PATH = "M7.5 1.5a4 4 0 0 1 4 4v3l2 3h-13l2-3v-3a4 4 0 0 1 4-4zM5.5 14.5h4"sv;
// A head over shoulders.
static constexpr auto PERSON_ICON_PATH = "M10 5a2.5 2.5 0 1 1-5 0a2.5 2.5 0 1 1 5 0M2.5 13.5c0-2.8 2.2-4.5 5-4.5s5 1.7 5 4.5"sv;
// A key: its bow at the top left, and two bits on the blade.
static constexpr auto PASSKEY_ICON_PATH = "M8 5a3 3 0 1 1-6 0a3 3 0 1 1 6 0M7.2 7.2l6.3 6.3M10.5 10.5l1.5-1.5M12.5 12.5l1.5-1.5"sv;
// A security key that stands on its end: the plug at the top, and the button to touch on its body.
static constexpr auto SECURITY_KEY_ICON_PATH = "M5.5 5.5h4a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-4a1 1 0 0 1-1-1v-7a1 1 0 0 1 1-1zM5.5 5.5v-4h4v4M9 10a1.5 1.5 0 1 1-3 0a1.5 1.5 0 1 1 3 0"sv;
static constexpr auto CHECK_CIRCLE_ICON_PATH = "M14.5 8a6.5 6.5 0 1 1-13 0a6.5 6.5 0 1 1 13 0M5.2 8.2l2 2 3.8-4.2"sv;
static constexpr auto STAR_ICON_PATH = "M7.5 1l2 4.4 4.8.6-3.5 3.3.9 4.8-4.2-2.4-4.2 2.4.9-4.8L.7 6l4.8-.6z"sv;
static constexpr auto CONTENT_BLOCKING_ICON_PATH = "M7.5 1.5l5 2v4.5c0 3-2 5-5 6.5-3-1.5-5-3.5-5-6.5V3.5z"sv;
static constexpr auto NEW_TAB_ICON_PATH = "M7.5 2.5v10M2.5 7.5h10"sv;
// A stack of cards: a wide one in front, with the edges of two shorter ones above it.
static constexpr auto ALL_TABS_ICON_PATH = "M2.5 6.5h11a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM3.5 4.5h9M5.5 2.5h5"sv;
static constexpr auto CLOSE_ICON_PATH = "M3.5 3.5l8 8M11.5 3.5l-8 8"sv;
static constexpr auto CHEVRON_DOWN_ICON_PATH = "M3.5 5.5l4 4 4-4"sv;
static constexpr auto CHEVRON_UP_ICON_PATH = "M3.5 9.5l4-4 4 4"sv;
static constexpr auto DOUBLE_CHEVRON_RIGHT_ICON_PATH = "M4.5 4.5l3 3-3 3M8.5 4.5l3 3-3 3"sv;
static constexpr auto SIDEBAR_ICON_PATH = "M3.5 2.5h9a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-9a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2zM5.5 2.5v11"sv;
static constexpr auto DOWNLOAD_ICON_PATH = "M7.5 1.5v9M3.5 6.5l4 4 4-4M2.5 13.5h10"sv;
static constexpr auto AUDIO_ICON_PATH = "M1.5 5.5h2l4-3v10l-4-3h-2zM10.5 4.5c1.3.8 2 1.8 2 3s-.7 2.2-2 3"sv;
static constexpr auto MUTED_ICON_PATH = "M1.5 5.5h2l4-3v10l-4-3h-2zM10.5 5.5l4 4M14.5 5.5l-4 4"sv;
static constexpr auto WARNING_ICON_PATH = "M7.5 1.5l6.5 12H1zM7.5 5.5v4M7.5 11.5v.01"sv;
static constexpr auto SEARCH_ICON_PATH = "M6.5 1.5a5 5 0 1 1 0 10a5 5 0 1 1 0-10zM10.5 10.5l4 4"sv;
static constexpr auto ZOOM_OUT_ICON_PATH = "M2.5 7.5h10"sv;
static constexpr auto PAUSE_ICON_PATH = "M5.5 3.5v9M10.5 3.5v9"sv;
static constexpr auto GLOBE_ICON_PATH = "M7.5 1.5a6 6 0 1 1 0 12a6 6 0 1 1 0-12zM1.5 7.5h12M7.5 1.5c-2 1.6-3 3.6-3 6s1 4.4 3 6M7.5 1.5c2 1.6 3 3.6 3 6s-1 4.4-3 6"sv;
static constexpr auto MOON_ICON_PATH = "M13.39 9.15A6 6 0 1 1 6.54 2.08a5 5 0 0 0 6.85 7.07z"sv;
static constexpr auto DOCUMENT_ICON_PATH = "M3.5 1.5h5l4 4v9h-9zM8.5 1.5v4h4"sv;
static constexpr auto COMPOSITOR_ICON_PATH = "M5.5 6.5h9v7h-9zM5.5 9.5h-4v-7h9v4"sv;
static constexpr auto HUB_ICON_PATH = "M7.5 5.5a2 2 0 1 1 0 4a2 2 0 1 1 0-4zM7.5 1.5a1 1 0 1 1 0 2a1 1 0 1 1 0-2zM7.5 11.5a1 1 0 1 1 0 2a1 1 0 1 1 0-2zM2.5 6.5a1 1 0 1 1 0 2a1 1 0 1 1 0-2zM12.5 6.5a1 1 0 1 1 0 2a1 1 0 1 1 0-2zM7.5 3.5v2M7.5 9.5v2M3.5 7.5h2M9.5 7.5h2"sv;
static constexpr auto IMAGE_ICON_PATH = "M2.5 2.5h11a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1zM1.5 11.5l3.5-3.5 5.5 5.5M9.5 9.5l2-2 3 3M10.5 5.5v.01"sv;
static constexpr auto FOLDER_ICON_PATH = "M2.5 2.5h3l2 2h6a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1z"sv;
static constexpr auto FEEDBACK_ICON_PATH = "M4 1.5h7a2.5 2.5 0 0 1 2.5 2.5v5a2.5 2.5 0 0 1-2.5 2.5H7l-2.5 2.5v-2.5H4a2.5 2.5 0 0 1-2.5-2.5V4a2.5 2.5 0 0 1 2.5-2.5zM7.5 4.25v1.6M7.5 8.75v.01"sv;
static constexpr auto EXTERNAL_LINK_ICON_PATH = "M6.5 2.5h-3a1 1 0 0 0-1 1v9a1 1 0 0 0 1 1h9a1 1 0 0 0 1-1v-3M9.5 1.5h4v4M13.5 1.5l-6 6"sv;

// The window controls of Windows and Linux are drawn on a 10 px box in the middle of the icon.
static constexpr auto WINDOW_MINIMIZE_ICON_PATH = "M3.5 8.5h9"sv;
static constexpr auto WINDOW_MAXIMIZE_ICON_PATH = "M4.5 3.5h7a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-7a1 1 0 0 1-1-1v-7a1 1 0 0 1 1-1z"sv;
static constexpr auto WINDOW_RESTORE_ICON_PATH = "M4.5 5.5h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-5a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM5.5 3.5h6a1 1 0 0 1 1 1v6"sv;
static constexpr auto WINDOW_CLOSE_ICON_PATH = "M3.5 3.5l9 9M12.5 3.5l-9 9"sv;

static constexpr qreal CHROME_ICON_STROKE_WIDTH = 1.5;
// Icons between these sizes all keep the same line, so that they look alike side by side. A smaller one needs a finer
// line to keep its shape, and a larger one is shown as a feature, with its line growing along with it.
static constexpr int SMALLEST_ICON_SIZE_WITH_FIXED_STROKE = 14;
static constexpr int LARGEST_ICON_SIZE_WITH_FIXED_STROKE = 18;
// How wide a one pixel line is drawn where it doesn't run along the pixels.
static constexpr qreal SLANTED_ONE_PIXEL_LINE_WIDTH = 1.3;
// How far across a curve can reach, in device pixels, and still be a rounded corner rather than a line of its own.
static constexpr qreal LONGEST_ROUNDED_CORNER = 5;

static StringView chrome_icon_path(ChromeIcon icon)
{
    switch (icon) {
    case ChromeIcon::Back:
        return BACK_ICON_PATH;
    case ChromeIcon::Forward:
        return FORWARD_ICON_PATH;
    case ChromeIcon::Reload:
        return RELOAD_ICON_PATH;
    case ChromeIcon::NewTab:
        return NEW_TAB_ICON_PATH;
    case ChromeIcon::AllTabs:
        return ALL_TABS_ICON_PATH;
    case ChromeIcon::Stop:
    case ChromeIcon::Close:
    case ChromeIcon::TabClose:
        return CLOSE_ICON_PATH;
    case ChromeIcon::Bookmarks:
    case ChromeIcon::Star:
    case ChromeIcon::StarFilled:
        return STAR_ICON_PATH;
    case ChromeIcon::Search:
        return SEARCH_ICON_PATH;
    case ChromeIcon::Globe:
        return GLOBE_ICON_PATH;
    case ChromeIcon::Folder:
        return FOLDER_ICON_PATH;
    case ChromeIcon::Document:
        return DOCUMENT_ICON_PATH;
    case ChromeIcon::Network:
        return GLOBE_ICON_PATH;
    case ChromeIcon::Compositor:
        return COMPOSITOR_ICON_PATH;
    case ChromeIcon::Hub:
        return HUB_ICON_PATH;
    case ChromeIcon::Image:
        return IMAGE_ICON_PATH;
    case ChromeIcon::Download:
        return DOWNLOAD_ICON_PATH;
    case ChromeIcon::Volume:
        return AUDIO_ICON_PATH;
    case ChromeIcon::VolumeMuted:
        return MUTED_ICON_PATH;
    case ChromeIcon::ChevronUp:
        return CHEVRON_UP_ICON_PATH;
    case ChromeIcon::ChevronDown:
        return CHEVRON_DOWN_ICON_PATH;
    case ChromeIcon::DoubleChevronRight:
        return DOUBLE_CHEVRON_RIGHT_ICON_PATH;
    case ChromeIcon::ContentBlocking:
        return CONTENT_BLOCKING_ICON_PATH;
    case ChromeIcon::Moon:
        return MOON_ICON_PATH;
    case ChromeIcon::SiteSettings:
        return SITE_SETTINGS_ICON_PATH;
    case ChromeIcon::Microphone:
        return MICROPHONE_ICON_PATH;
    case ChromeIcon::Location:
        return LOCATION_ICON_PATH;
    case ChromeIcon::Notifications:
        return NOTIFICATIONS_ICON_PATH;
    case ChromeIcon::Person:
        return PERSON_ICON_PATH;
    case ChromeIcon::Passkey:
        return PASSKEY_ICON_PATH;
    case ChromeIcon::SecurityKey:
        return SECURITY_KEY_ICON_PATH;
    case ChromeIcon::CheckCircle:
        return CHECK_CIRCLE_ICON_PATH;
    case ChromeIcon::Warning:
        return WARNING_ICON_PATH;
    case ChromeIcon::Pause:
        return PAUSE_ICON_PATH;
    case ChromeIcon::Feedback:
        return FEEDBACK_ICON_PATH;
    case ChromeIcon::ExternalLink:
        return EXTERNAL_LINK_ICON_PATH;
    case ChromeIcon::ZoomOut:
        return ZOOM_OUT_ICON_PATH;
    case ChromeIcon::Sidebar:
    case ChromeIcon::SidebarRight:
        return SIDEBAR_ICON_PATH;
    case ChromeIcon::WindowMinimize:
        return WINDOW_MINIMIZE_ICON_PATH;
    case ChromeIcon::WindowMaximize:
        return WINDOW_MAXIMIZE_ICON_PATH;
    case ChromeIcon::WindowRestore:
        return WINDOW_RESTORE_ICON_PATH;
    case ChromeIcon::WindowClose:
        return WINDOW_CLOSE_ICON_PATH;
    }
    VERIFY_NOT_REACHED();
}

// The right sidebar's icon is the left one's, mirrored.
static QPainterPath mirrored_for_right_sidebar(ChromeIcon icon, QPainterPath path)
{
    if (icon != ChromeIcon::SidebarRight)
        return path;
    QTransform transform;
    transform.translate(CHROME_ICON_CANVAS_SIZE, 0);
    transform.scale(-1, 1);
    return transform.map(path);
}

static QPainterPath const& chrome_icon_painter_path(ChromeIcon icon)
{
    static HashMap<ChromeIcon, QPainterPath> s_paths;
    return s_paths.ensure(icon, [&] {
        return mirrored_for_right_sidebar(icon, parse_svg_path(chrome_icon_path(icon)));
    });
}

static bool coordinates_match(qreal a, qreal b)
{
    return abs(a - b) < 0.001;
}

// The lines of an icon that fitting should put on the pixel grid, as positions along each axis.
struct IconStems {
    Vector<qreal> vertical;
    Vector<qreal> horizontal;
    // Whether the icon has nothing but 45 degree lines, as a cross or a chevron does.
    bool is_diagonal { true };
};

static IconStems find_icon_stems(QPainterPath const& path)
{
    IconStems stems;
    auto add = [](Vector<qreal>& positions, qreal position) {
        if (!positions.find_if([&](auto other) { return coordinates_match(other, position); }).is_end())
            return;
        positions.append(position);
    };
    auto point_at = [&](int index) -> QPointF { return path.elementAt(index); };

    auto element_count = path.elementCount();
    for (int index = 0; index < element_count; ++index) {
        auto const& element = path.elementAt(index);

        if (element.isLineTo()) {
            auto from = point_at(index - 1);
            auto to = point_at(index);
            auto is_vertical = coordinates_match(from.x(), to.x());
            auto is_horizontal = coordinates_match(from.y(), to.y());
            if (is_vertical)
                add(stems.vertical, to.x());
            if (is_horizontal)
                add(stems.horizontal, to.y());
            if (!coordinates_match(abs(from.x() - to.x()), abs(from.y() - to.y())))
                stems.is_diagonal = false;

            // The ends of a line that stops in the open are put on the grid too, so that it ends on a whole pixel.
            // NB: A dot is a line too short to see, and has to keep its length.
            auto is_dot = abs(from.x() - to.x()) + abs(from.y() - to.y()) < 0.5;
            auto starts_open = path.elementAt(index - 1).isMoveTo();
            auto ends_open = index + 1 == element_count || path.elementAt(index + 1).isMoveTo();
            if (is_vertical) {
                if (starts_open)
                    add(stems.horizontal, from.y());
                if (ends_open && !is_dot)
                    add(stems.horizontal, to.y());
            }
            if (is_horizontal) {
                if (starts_open)
                    add(stems.vertical, from.x());
                if (ends_open && !is_dot)
                    add(stems.vertical, to.x());
            }
        } else if (element.isCurveTo()) {
            // The turning points of a curve count too: the top of a circle should be as crisp as the top of a square.
            stems.is_diagonal = false;
            auto from = point_at(index - 1);
            auto first_control = point_at(index);
            auto second_control = point_at(index + 1);
            auto to = point_at(index + 2);
            if (coordinates_match(from.x(), first_control.x()))
                add(stems.vertical, from.x());
            if (coordinates_match(from.y(), first_control.y()))
                add(stems.horizontal, from.y());
            if (coordinates_match(to.x(), second_control.x()))
                add(stems.vertical, to.x());
            if (coordinates_match(to.y(), second_control.y()))
                add(stems.horizontal, to.y());
            index += 2;
        }
    }

    quick_sort(stems.vertical);
    quick_sort(stems.horizontal);
    return stems;
}

// A place on one axis of an icon, and where fitting moves it to.
struct FittedPosition {
    qreal from { 0 };
    qreal to { 0 };
};

// Lines of an odd width need their center on a half pixel to fill whole pixels, and lines of an even width on a whole
// one.
static qreal pixel_phase(int stroke_width)
{
    return stroke_width % 2 == 0 ? 0 : 0.5;
}

static qreal nearest_on_pixel_grid(qreal position, int stroke_width)
{
    auto phase = pixel_phase(stroke_width);
    return floor(position - phase + 0.5) + phase;
}

static bool lies_between_pixel_grid_positions(qreal position, int stroke_width)
{
    return coordinates_match(position - pixel_phase(stroke_width) + 0.5, round(position - pixel_phase(stroke_width) + 0.5));
}

static Vector<FittedPosition> fit_stems_to_pixels(Vector<qreal> const& stems, qreal scale, int stroke_width, int canvas_size)
{
    Vector<FittedPosition> fitted;
    if (stems.is_empty())
        return fitted;

    // The grid of an icon on half pixels falls exactly between the pixels at 2x. Moving the whole icon, toward the
    // middle of the canvas, keeps its lines evenly spaced, where moving each one to its nearest place would not.
    size_t lines_between_positions = 0;
    for (auto stem : stems) {
        if (lies_between_pixel_grid_positions(stem * scale, stroke_width))
            ++lines_between_positions;
    }
    qreal shift = 0;
    if (lines_between_positions * 2 >= stems.size()) {
        auto middle = (stems.first() + stems.last()) * scale / 2;
        shift = middle < canvas_size / 2.0 ? 0.5 : -0.5;
    }

    for (auto stem : stems) {
        auto position = stem * scale;
        auto target = nearest_on_pixel_grid(position + shift, stroke_width);
        // Lines that had room between them keep at least a pixel of it.
        if (!fitted.is_empty()) {
            auto const& previous = fitted.last();
            if (position - previous.from >= stroke_width + 0.5 && target - previous.to < stroke_width + 1)
                target = previous.to + stroke_width + 1;
            target = max(target, previous.to);
        }
        fitted.append({ position, target });
    }
    return fitted;
}

// Everything between two fitted places moves along with them, and whatever is beyond them moves as the nearest does.
static qreal apply_fitted_positions(Vector<FittedPosition> const& fitted, qreal position)
{
    if (fitted.is_empty())
        return position;
    if (position <= fitted.first().from)
        return position + fitted.first().to - fitted.first().from;
    if (position >= fitted.last().from)
        return position + fitted.last().to - fitted.last().from;
    for (size_t index = 1; index < fitted.size(); ++index) {
        auto const& start = fitted[index - 1];
        auto const& end = fitted[index];
        if (position > end.from)
            continue;
        return start.to + ((position - start.from) / (end.from - start.from) * (end.to - start.to));
    }
    VERIFY_NOT_REACHED();
}

// A 45 degree line is only clean if it runs through the corners of pixels, and it only stays at 45 degrees if the icon
// is scaled evenly. So an icon of nothing else is resized as a whole, to a length that keeps its corners on the grid.
static QTransform transform_for_diagonal_icon(QPainterPath const& path, qreal scale, int stroke_width, int canvas_size)
{
    auto bounds = path.boundingRect();
    auto length = max(bounds.width(), bounds.height());

    // An even length puts the middle of a cross on one spot, so that its lines cross in a single place.
    auto fitted_length = max(2.0, round(length * scale / 2) * 2);
    auto fitted_scale = fitted_length / length;

    auto left = nearest_on_pixel_grid((canvas_size - (bounds.width() * fitted_scale)) / 2, stroke_width);
    auto top = nearest_on_pixel_grid((canvas_size - (bounds.height() * fitted_scale)) / 2, stroke_width);

    QTransform transform;
    transform.translate(left - (bounds.left() * fitted_scale), top - (bounds.top() * fitted_scale));
    transform.scale(fitted_scale, fitted_scale);
    return transform;
}

// Moves a part of an icon, or the whole of it, as fitting the icon to the pixels moves it.
static QPainterPath fit_icon_part_to_pixels(QPainterPath const& icon_path, QPainterPath const& part, qreal scale, int stroke_width, int canvas_size)
{
    auto stems = find_icon_stems(icon_path);
    if (stems.is_diagonal)
        return transform_for_diagonal_icon(icon_path, scale, stroke_width, canvas_size).map(part);

    auto vertical = fit_stems_to_pixels(stems.vertical, scale, stroke_width, canvas_size);
    auto horizontal = fit_stems_to_pixels(stems.horizontal, scale, stroke_width, canvas_size);

    QPainterPath fitted = part;
    for (int index = 0; index < fitted.elementCount(); ++index) {
        auto const& element = fitted.elementAt(index);
        fitted.setElementPositionAt(index, apply_fitted_positions(vertical, element.x * scale), apply_fitted_positions(horizontal, element.y * scale));
    }
    return fitted;
}

// Puts the lines of an icon on the pixel grid of the size it is shown at, the way a font's hints do for its letters.
static QPainterPath fit_icon_path_to_pixels(QPainterPath const& path, qreal scale, int stroke_width, int canvas_size)
{
    return fit_icon_part_to_pixels(path, path, scale, stroke_width, canvas_size);
}

static qreal chrome_icon_stroke_width(int logical_size)
{
    if (logical_size > LARGEST_ICON_SIZE_WITH_FIXED_STROKE)
        return CHROME_ICON_STROKE_WIDTH * logical_size / CHROME_ICON_CANVAS_SIZE;
    if (logical_size < SMALLEST_ICON_SIZE_WITH_FIXED_STROKE)
        return CHROME_ICON_STROKE_WIDTH * logical_size / SMALLEST_ICON_SIZE_WITH_FIXED_STROKE;
    return CHROME_ICON_STROKE_WIDTH;
}

// The straight lines of an icon that run along the pixel grid, and everything else.
struct IconLines {
    QPainterPath straight;
    QPainterPath slanted;
};

static IconLines split_icon_lines(QPainterPath const& path)
{
    IconLines lines;
    auto start_at = [](QPainterPath& part, QPointF point) {
        if (part.elementCount() == 0 || part.currentPosition() != point)
            part.moveTo(point);
    };

    for (int index = 1; index < path.elementCount(); ++index) {
        auto const& element = path.elementAt(index);
        QPointF from = path.elementAt(index - 1);
        QPointF to = element;
        if (element.isLineTo()) {
            auto& part = coordinates_match(from.x(), to.x()) || coordinates_match(from.y(), to.y()) ? lines.straight : lines.slanted;
            start_at(part, from);
            part.lineTo(to);
        } else if (element.isCurveTo()) {
            // NB: A small rounded corner belongs to the lines on either side of it. It is too short to look thin.
            QPointF end = path.elementAt(index + 2);
            auto& part = abs(from.x() - end.x()) + abs(from.y() - end.y()) < LONGEST_ROUNDED_CORNER ? lines.straight : lines.slanted;
            start_at(part, from);
            part.cubicTo(to, path.elementAt(index + 1), end);
            index += 2;
        }
    }
    return lines;
}

// The outline of an icon's lines. A one pixel line fills its pixels when it runs along them, but it spreads thin over a
// slant or a curve, so these are drawn a little wider to match.
static QPainterPath icon_outline(QPainterPath const& path, int stroke_width)
{
    QPainterPathStroker stroker;
    stroker.setWidth(stroke_width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    if (stroke_width > 1)
        return stroker.createStroke(path);

    auto lines = split_icon_lines(path);

    // NB: These lines end on the grid, so a rounded end or corner would leave a soft pixel behind.
    stroker.setCapStyle(Qt::SquareCap);
    stroker.setJoinStyle(Qt::MiterJoin);
    auto outline = stroker.createStroke(lines.straight);

    stroker.setWidth(SLANTED_ONE_PIXEL_LINE_WIDTH);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    outline.addPath(stroker.createStroke(lines.slanted));
    return outline;
}

// The icon is drawn from its vector at the size and device pixel ratio it is shown at, never scaled from a picture.
// Draws an icon from the outline of its lines, which it is given in pixels of the screen for the scale of the icon,
// the width of its lines and the size of its canvas. A margin around the canvas leaves room for lines that move past
// its edges.
template<typename OutlineFunction>
static QPixmap create_chrome_icon_pixmap(QColor color, qreal device_pixel_ratio, int logical_size, OutlineFunction outline_for, int margin_in_pixels = 0)
{
    auto canvas_size = physical_size_for_device_pixel_ratio({ logical_size, logical_size }, device_pixel_ratio);
    QPixmap pixmap(canvas_size + QSize(2 * margin_in_pixels, 2 * margin_in_pixels));
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // NB: Everything below is measured in pixels of the screen.
    painter.scale(1 / device_pixel_ratio, 1 / device_pixel_ratio);
    painter.translate(margin_in_pixels, margin_in_pixels);

    // An icon is drawn in one opaque color, then made as translucent as its color as a whole, so that where its lines
    // cross they don't add up to a brighter spot.
    auto opaque_color = color;
    opaque_color.setAlpha(255);

    // A line of a whole number of pixels can fill them exactly. Rounding down keeps a 1x line at one pixel, the most
    // that leaves room inside an icon of this size.
    auto stroke_width = max(1, static_cast<int>(floor((chrome_icon_stroke_width(logical_size) * device_pixel_ratio) + 0.25)));
    auto scale = logical_size * device_pixel_ratio / CHROME_ICON_CANVAS_SIZE;

    // Fill the combined outline once so antialiased edges do not accumulate where lines meet.
    QPainterPath outline = outline_for(scale, stroke_width, canvas_size.width());
    outline.setFillRule(Qt::WindingFill);
    painter.fillPath(outline.simplified(), opaque_color);

    if (color.alpha() != 255) {
        painter.resetTransform();
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(QRect(QPoint(), pixmap.size()), QColor(0, 0, 0, color.alpha()));
    }

    return pixmap;
}

static QPixmap create_chrome_icon_pixmap(ChromeIcon icon, QPainterPath const& path, QColor color, qreal device_pixel_ratio, int logical_size)
{
    return create_chrome_icon_pixmap(color, device_pixel_ratio, logical_size, [&](qreal scale, int stroke_width, int canvas_size) {
        auto fitted_path = fit_icon_path_to_pixels(path, scale, stroke_width, canvas_size);
        auto outline = icon_outline(fitted_path, stroke_width);
        if (icon == ChromeIcon::StarFilled)
            outline.addPath(fitted_path);
        return outline;
    });
}

// Draws a chrome icon from its vector for each size and device pixel ratio it is asked for.
class ChromeIconEngine : public QIconEngine {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct Colors {
        QColor normal;
        QColor active;
        QColor disabled;
        QColor selected;
    };

    ChromeIconEngine(ChromeIcon icon, QPainterPath path, Colors colors, int logical_size)
        : m_icon(icon)
        , m_path(AK::move(path))
        , m_colors(AK::move(colors))
        , m_logical_size(logical_size)
    {
    }

    virtual QIconEngine* clone() const override { return new ChromeIconEngine(m_icon, m_path, m_colors, m_logical_size); }

    virtual QSize actualSize(QSize const& size, QIcon::Mode, QIcon::State) override
    {
        auto side = min(size.width(), size.height());
        return { side, side };
    }

    virtual QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return { QSize(m_logical_size, m_logical_size) }; }

    virtual QPixmap pixmap(QSize const& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1);
    }

    virtual QPixmap scaledPixmap(QSize const& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        auto side = min(size.width(), size.height());
        if (side <= 0)
            return {};

        // OPTIMIZATION: Fitting an icon to the pixels costs far more than drawing the result, and the same few pictures
        //               are asked for on every repaint.
        for (auto const& entry : m_pixmaps) {
            if (entry.side == side && entry.scale == scale && entry.mode == mode)
                return entry.pixmap;
        }
        auto pixmap = create_icon_pixmap(color_for_mode(mode), scale, side);
        m_pixmaps.append({ side, scale, mode, pixmap });
        return pixmap;
    }

    virtual void paint(QPainter* painter, QRect const& rect, QIcon::Mode mode, QIcon::State state) override
    {
        auto side = min(rect.width(), rect.height());
        auto device_pixel_ratio = painter->device() ? painter->device()->devicePixelRatio() : 1;
        auto pixmap = scaledPixmap(QSize(side, side), mode, state, device_pixel_ratio);
        auto position = rect.topLeft() + QPoint((rect.width() - side) / 2, (rect.height() - side) / 2);
        painter->drawPixmap(position, pixmap);
    }

protected:
    virtual QPixmap create_icon_pixmap(QColor color, qreal device_pixel_ratio, int logical_size) const
    {
        return create_chrome_icon_pixmap(m_icon, m_path, color, device_pixel_ratio, logical_size);
    }

    Colors const& colors() const { return m_colors; }

private:
    QColor color_for_mode(QIcon::Mode mode) const
    {
        switch (mode) {
        case QIcon::Active:
            return m_colors.active;
        case QIcon::Disabled:
            return m_colors.disabled;
        case QIcon::Selected:
            return m_colors.selected;
        case QIcon::Normal:
            break;
        }
        return m_colors.normal;
    }

    struct CachedPixmap {
        int side { 0 };
        qreal scale { 1 };
        QIcon::Mode mode { QIcon::Normal };
        QPixmap pixmap;
    };

    ChromeIcon m_icon;
    QPainterPath m_path;
    Colors m_colors;
    int m_logical_size { CHROME_ICON_CANVAS_SIZE };
    Vector<CachedPixmap> m_pixmaps;
};

QPixmap create_chrome_icon_pixmap(ChromeIcon icon, QColor color, qreal device_pixel_ratio)
{
    return create_chrome_icon_pixmap(icon, chrome_icon_painter_path(icon), color, device_pixel_ratio, CHROME_ICON_CANVAS_SIZE);
}

static ChromeIconEngine::Colors chrome_icon_colors(ChromeIcon icon, QPalette const& palette, ChromeStyle::WindowActivity activity)
{
    auto const& chrome = Theme::chrome(palette);

    // Toolbar buttons take the icon color. The controls inside the address field and the tabs are quieter.
    auto normal = chrome.icon;
    switch (icon) {
    case ChromeIcon::Close:
    case ChromeIcon::TabClose:
    case ChromeIcon::ContentBlocking:
    case ChromeIcon::Star:
    case ChromeIcon::StarFilled:
    case ChromeIcon::SiteSettings:
    case ChromeIcon::Search:
    case ChromeIcon::Globe:
    case ChromeIcon::Volume:
    case ChromeIcon::VolumeMuted:
        normal = chrome.muted;
        break;
    default:
        break;
    }
    if (icon == ChromeIcon::StarFilled)
        normal = chrome.icon;
    // An inactive window's chrome icons drop to the muted color.
    if (activity == ChromeStyle::WindowActivity::Inactive)
        normal = chrome.muted;

    return { normal, chrome.icon, chrome.disabled, chrome.icon };
}

static QIcon create_chrome_icon(ChromeIcon icon, QPainterPath const& path, QPalette const& palette, ChromeStyle::WindowActivity activity, int logical_size)
{
    return QIcon(new ChromeIconEngine(icon, path, chrome_icon_colors(icon, palette, activity), logical_size));
}

QIcon create_chrome_icon(ChromeIcon icon, QPalette const& palette, ChromeStyle::WindowActivity activity, int logical_size)
{
    return create_chrome_icon(icon, chrome_icon_painter_path(icon), palette, activity, logical_size);
}

// The icon is two lines, each with a knob on it, the top one to the right and the bottom one to the left. The knobs
// slide past each other as the travel goes from 0 to 1, and each line breaks around its knob.
QIcon create_site_settings_icon(QPalette const& palette, qreal knob_travel)
{
    static constexpr qreal LINE_START = 1.5;
    static constexpr qreal LINE_END = 13.5;
    static constexpr qreal KNOB_RADIUS = 2;
    static constexpr qreal KNOB_TRAVEL = 6;

    QPainterPath path;
    auto add_line_with_knob = [&](qreal y, qreal knob_x) {
        if (knob_x - KNOB_RADIUS > LINE_START) {
            path.moveTo(LINE_START, y);
            path.lineTo(knob_x - KNOB_RADIUS, y);
        }
        if (knob_x + KNOB_RADIUS < LINE_END) {
            path.moveTo(knob_x + KNOB_RADIUS, y);
            path.lineTo(LINE_END, y);
        }
        path.addEllipse(QPointF(knob_x, y), KNOB_RADIUS, KNOB_RADIUS);
    };
    add_line_with_knob(4.5, 10.5 - (KNOB_TRAVEL * knob_travel));
    add_line_with_knob(11.5, 4.5 + (KNOB_TRAVEL * knob_travel));

    return create_chrome_icon(ChromeIcon::SiteSettings, path, palette, ChromeStyle::WindowActivity::Active, CHROME_ICON_CANVAS_SIZE);
}

// What is left of a path of one line or curve after another once the given part of its length is cut from its start.
static QPainterPath cut_from_start_of_path(QPainterPath const& path, qreal cut_part)
{
    auto cut_length = path.length() * cut_part;
    QPainterPath result;
    QPointF current = path.elementAt(0);
    qreal length_so_far = 0;
    for (int index = 1; index < path.elementCount(); ++index) {
        auto const& element = path.elementAt(index);
        QPainterPath segment;
        segment.moveTo(current);
        if (element.isCurveTo()) {
            QPointF control1 = element;
            QPointF control2 = path.elementAt(index + 1);
            QPointF end = path.elementAt(index + 2);
            segment.cubicTo(control1, control2, end);
            auto segment_length = segment.length();
            if (length_so_far + segment_length > cut_length) {
                if (result.elementCount() == 0) {
                    // NB: Split the curve where the cut falls, keeping its second part.
                    auto t = segment.percentAtLength(cut_length - length_so_far);
                    auto lerp = [](QPointF a, QPointF b, qreal t) { return a + ((b - a) * t); };
                    auto p01 = lerp(current, control1, t);
                    auto p12 = lerp(control1, control2, t);
                    auto p23 = lerp(control2, end, t);
                    auto p012 = lerp(p01, p12, t);
                    auto p123 = lerp(p12, p23, t);
                    result.moveTo(lerp(p012, p123, t));
                    result.cubicTo(p123, p23, end);
                } else {
                    result.cubicTo(control1, control2, end);
                }
            }
            length_so_far += segment_length;
            current = end;
            index += 2;
            continue;
        }
        QPointF end = element;
        segment.lineTo(end);
        auto segment_length = segment.length();
        if (length_so_far + segment_length > cut_length) {
            if (result.elementCount() == 0)
                result.moveTo(current + ((end - current) * ((cut_length - length_so_far) / segment_length)));
            result.lineTo(end);
        }
        length_so_far += segment_length;
        current = end;
    }
    return result;
}

// The reload arrow turns a quarter turn clockwise while its arc reels itself into the arrowhead, whose legs then pull
// into their corner. The cross draws itself from there, one line after the other: the first from the bottom right,
// where the arrowhead ended up, and the second from the top right.
class ReloadToStopIconEngine final : public ChromeIconEngine {
public:
    AK_ALLOC_WITH_KMALLOC;

    ReloadToStopIconEngine(Colors colors, int logical_size, qreal progress)
        : ChromeIconEngine(ChromeIcon::Reload, {}, AK::move(colors), logical_size)
        , m_logical_size(logical_size)
        , m_progress(progress)
    {
        build_paths();
    }

    virtual QIconEngine* clone() const override { return new ReloadToStopIconEngine(colors(), m_logical_size, m_progress); }

    // NB: The pixmap reaches past the icon by its margin on each side.
    virtual void paint(QPainter* painter, QRect const& rect, QIcon::Mode mode, QIcon::State state) override
    {
        auto side = min(rect.width(), rect.height());
        auto device_pixel_ratio = painter->device() ? painter->device()->devicePixelRatio() : 1;
        auto pixmap = scaledPixmap(QSize(side, side), mode, state, device_pixel_ratio);
        auto margin = margin_in_pixels(device_pixel_ratio) / device_pixel_ratio;
        auto position = QPointF(rect.topLeft() + QPoint((rect.width() - side) / 2, (rect.height() - side) / 2)) - QPointF(margin, margin);
        painter->drawPixmap(position, pixmap);
    }

private:
    void build_paths()
    {
        static auto const arc = parse_svg_path("M13.41 9a5.5 5.5 0 0 1-5.41 4.5a5.5 5.5 0 1 1 3.89-9.39L13.5 5.5"sv);
        static constexpr QPointF ARROWHEAD_CORNER { 13.5, 5.5 };
        static constexpr QPointF ARROWHEAD_LEG_ENDS[] { { 13.5, 2 }, { 10, 5.5 } };
        static auto const exit_easing = Motion::exit_easing();
        static auto const enter_easing = Motion::enter_easing();

        auto part = [&](qreal start, qreal end) { return clamp((m_progress - start) / (end - start), 0.0, 1.0); };
        auto reeled_in = exit_easing.valueForProgress(part(0, 0.45));
        auto legs_pulled_in = exit_easing.valueForProgress(part(0.325, 0.45));
        auto first_line_drawn = enter_easing.valueForProgress(part(0.425, 0.8));
        auto second_line_drawn = enter_easing.valueForProgress(part(0.55, 1));

        QPainterPath arrow;
        if (reeled_in < 1)
            arrow = cut_from_start_of_path(arc, reeled_in);
        if (legs_pulled_in < 1) {
            for (auto leg_end : ARROWHEAD_LEG_ENDS) {
                arrow.moveTo(ARROWHEAD_CORNER);
                arrow.lineTo(ARROWHEAD_CORNER + ((leg_end - ARROWHEAD_CORNER) * (1 - legs_pulled_in)));
            }
        }
        m_arrow = arrow;
        m_arrow_turn_degrees = 90 * reeled_in;

        // NB: A line drawn only a sliver of its length would show as a dot.
        static constexpr qreal SHORTEST_DRAWN_PART = 0.02;
        auto add_line = [&](QPointF from, QPointF to, qreal drawn) {
            if (drawn <= SHORTEST_DRAWN_PART)
                return;
            m_cross.moveTo(from);
            m_cross.lineTo(from + ((to - from) * drawn));
        };
        add_line({ 11.5, 11.5 }, { 3.5, 3.5 }, first_line_drawn);
        add_line({ 11.5, 3.5 }, { 3.5, 11.5 }, second_line_drawn);
    }

    // The arrowhead swings past the corners of the icon as the arrow turns.
    static int margin_in_pixels(qreal device_pixel_ratio)
    {
        static constexpr qreal MARGIN = 2;
        return static_cast<int>(ceil(MARGIN * device_pixel_ratio));
    }

    // The arrow is fitted to the pixels as the reload icon is, and the cross as the stop icon is, so that the morph
    // starts and ends just where the two icons take over. The arrow turns after it is fitted, so that its lines move
    // smoothly rather than jump from one pixel to the next.
    virtual QPixmap create_icon_pixmap(QColor color, qreal device_pixel_ratio, int logical_size) const override
    {
        static constexpr QPointF ARROW_TURN_CENTER { 8, 8 };
        auto outline_for = [&](qreal scale, int stroke_width, int canvas_size) {
            auto const& reload_path = chrome_icon_painter_path(ChromeIcon::Reload);
            QPainterPath turn_center;
            turn_center.moveTo(ARROW_TURN_CENTER);
            auto fitted_turn_center = QPointF(fit_icon_part_to_pixels(reload_path, turn_center, scale, stroke_width, canvas_size).elementAt(0));
            QTransform turn;
            turn.translate(fitted_turn_center.x(), fitted_turn_center.y());
            turn.rotate(m_arrow_turn_degrees);
            turn.translate(-fitted_turn_center.x(), -fitted_turn_center.y());
            auto outline = icon_outline(turn.map(fit_icon_part_to_pixels(reload_path, m_arrow, scale, stroke_width, canvas_size)), stroke_width);

            auto const& stop_path = chrome_icon_painter_path(ChromeIcon::Stop);
            outline.addPath(icon_outline(fit_icon_part_to_pixels(stop_path, m_cross, scale, stroke_width, canvas_size), stroke_width));
            return outline;
        };
        return create_chrome_icon_pixmap(color, device_pixel_ratio, logical_size, outline_for, margin_in_pixels(device_pixel_ratio));
    }

    int m_logical_size { CHROME_ICON_CANVAS_SIZE };
    qreal m_progress { 0 };
    QPainterPath m_arrow;
    qreal m_arrow_turn_degrees { 0 };
    QPainterPath m_cross;
};

QIcon create_reload_to_stop_icon(QPalette const& palette, ChromeStyle::WindowActivity activity, qreal progress, int logical_size)
{
    return QIcon(new ReloadToStopIconEngine(chrome_icon_colors(ChromeIcon::Reload, palette, activity), logical_size, progress));
}

#if !defined(AK_OS_MACOS)
// NB: Pages are only translated on macOS, where the system draws the icon; see TranslateIconMac.mm.
QIcon create_translate_icon(QPalette const& palette, bool)
{
    return create_chrome_icon(ChromeIcon::Globe, palette);
}
#endif

QIcon create_chrome_icon(ChromeIcon icon, QColor const& color)
{
    return QIcon(new ChromeIconEngine(icon, chrome_icon_painter_path(icon), { color, color, color, color }, CHROME_ICON_CANVAS_SIZE));
}

QSize physical_size_for_device_pixel_ratio(QSize size, qreal device_pixel_ratio)
{
    return {
        static_cast<int>(ceil(size.width() * device_pixel_ratio)),
        static_cast<int>(ceil(size.height() * device_pixel_ratio)),
    };
}

// The logomark traces x = sin 2t, y = sin 3t, scaled 179 : 145, on a box of 22 by 18.4 around its middle.
static constexpr auto LOGOMARK_PATH = "M0 0 L1.39 1.68 L2.76 3.29 L4.07 4.76 L5.30 6.02 L6.43 7.02 L7.43 7.70 L8.29 8.06 L8.99 8.06 L9.51 7.70 L9.85 7.02 L9.99 6.02 L9.95 4.76 L9.70 3.29 L9.27 1.68 L8.66 0 L7.88 -1.68 L6.95 -3.29 L5.88 -4.76 L4.69 -6.02 L3.42 -7.02 L2.08 -7.70 L0.70 -8.06 L-0.70 -8.06 L-2.08 -7.70 L-3.42 -7.02 L-4.69 -6.02 L-5.88 -4.76 L-6.95 -3.29 L-7.88 -1.68 L-8.66 0 L-9.27 1.68 L-9.70 3.29 L-9.95 4.76 L-9.99 6.02 L-9.85 7.02 L-9.51 7.70 L-8.99 8.06 L-8.29 8.06 L-7.43 7.70 L-6.43 7.02 L-5.30 6.02 L-4.07 4.76 L-2.76 3.29 L-1.39 1.68 L0 0 L1.39 -1.68 L2.76 -3.29 L4.07 -4.76 L5.30 -6.02 L6.43 -7.02 L7.43 -7.70 L8.29 -8.06 L8.99 -8.06 L9.51 -7.70 L9.85 -7.02 L9.99 -6.02 L9.95 -4.76 L9.70 -3.29 L9.27 -1.68 L8.66 0 L7.88 1.68 L6.95 3.29 L5.88 4.76 L4.69 6.02 L3.42 7.02 L2.08 7.70 L0.70 8.06 L-0.70 8.06 L-2.08 7.70 L-3.42 7.02 L-4.69 6.02 L-5.88 4.76 L-6.95 3.29 L-7.88 1.68 L-8.66 0 L-9.27 -1.68 L-9.70 -3.29 L-9.95 -4.76 L-9.99 -6.02 L-9.85 -7.02 L-9.51 -7.70 L-8.99 -8.06 L-8.29 -8.06 L-7.43 -7.70 L-6.43 -7.02 L-5.30 -6.02 L-4.07 -4.76 L-2.76 -3.29 L-1.39 -1.68 Z"sv;
static constexpr QRectF LOGOMARK_BOUNDS { -11, -9.2, 22, 18.4 };

void paint_logomark(QPainter& painter, QRectF const& rect, QColor const& color, qreal stroke_width)
{
    static auto const path = parse_svg_path(LOGOMARK_PATH);

    // NB: The stroke width is given on the logomark's own box, as the design gives it, so it scales along with the path.
    auto scale = min(rect.width() / LOGOMARK_BOUNDS.width(), rect.height() / LOGOMARK_BOUNDS.height());

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(rect.center());
    painter.scale(scale, scale);
    painter.translate(-LOGOMARK_BOUNDS.center());
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, stroke_width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.restore();
}

}
