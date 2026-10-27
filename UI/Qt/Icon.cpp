/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibCore/Resource.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
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

QIcon icon_from_png(ReadonlyBytes favicon_png, int logical_size)
{
    QPixmap pixmap;
    if (!pixmap.loadFromData(favicon_png.data(), static_cast<uint>(favicon_png.size()), "PNG"))
        return {};

    QIcon icon;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS) {
        auto size = logical_size * device_pixel_ratio;
        auto scaled_pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled_pixmap.setDevicePixelRatio(device_pixel_ratio);
        icon.addPixmap(scaled_pixmap);
    }
    return icon;
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

QIcon favicon_with_rounded_corners(QIcon const& favicon, int logical_size)
{
    if (favicon.isNull())
        return {};

    QIcon rounded;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS)
        rounded.addPixmap(favicon_with_rounded_corners(favicon.pixmap(QSize(logical_size, logical_size), device_pixel_ratio)));
    return rounded;
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
            return atan2((ux * vy) - (uy * vx), (ux * vx) + (uy * vy));
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

// The icon set of the chrome: 16 px on a 16 px grid, stroked 1.5 px wide with round caps and joins.
static constexpr auto BACK_ICON_PATH = "M10 3.5L5.5 8l4.5 4.5"sv;
static constexpr auto FORWARD_ICON_PATH = "M6 3.5L10.5 8 6 12.5"sv;
static constexpr auto RELOAD_ICON_PATH = "M12.89 9.04A5 5 0 1 1 8 3c1.4 0 2.74.56 3.74 1.52L13 5.8M13 2.8v3h-2.8"sv;
static constexpr auto STOP_ICON_PATH = "M4 4l8 8M12 4l-8 8"sv;
static constexpr auto SITE_SETTINGS_ICON_PATH = "M2.5 5h6M12.5 5h1M2.5 11h1M7.5 11h6M12.1 5a1.6 1.6 0 1 1-3.2 0a1.6 1.6 0 1 1 3.2 0M7.1 11a1.6 1.6 0 1 1-3.2 0a1.6 1.6 0 1 1 3.2 0"sv;
static constexpr auto MICROPHONE_ICON_PATH = "M8 2.5a2 2 0 0 1 2 2v3.5a2 2 0 0 1-4 0V4.5a2 2 0 0 1 2-2zM4.5 7.5a3.5 3.5 0 0 0 7 0M8 11v2.5"sv;
static constexpr auto LOCATION_ICON_PATH = "M8 14.2C5.1 10.9 3.6 8.6 3.6 6.7a4.4 4.4 0 0 1 8.8 0c0 1.9-1.5 4.2-4.4 7.5zM9.6 6.7a1.6 1.6 0 1 1-3.2 0a1.6 1.6 0 1 1 3.2 0"sv;
static constexpr auto SELECT_AREA_ICON_PATH = "M2 5V3a1 1 0 0 1 1-1h2M11 2h2a1 1 0 0 1 1 1v2M14 11v2a1 1 0 0 1-1 1h-2M5 14H3a1 1 0 0 1-1-1v-2"sv;
static constexpr auto CHECK_CIRCLE_ICON_PATH = "M14.5 8a6.5 6.5 0 1 1-13 0a6.5 6.5 0 1 1 13 0M5.2 8.2l2 2 3.8-4.2"sv;
static constexpr auto STAR_ICON_PATH = "M8 2.3l1.75 3.55 3.9.57-2.83 2.75.67 3.88L8 11.22l-3.49 1.83.67-3.88L2.35 6.42l3.9-.57z"sv;
static constexpr auto CONTENT_BLOCKING_ICON_PATH = "M8 1.8l5 1.9v4c0 3.1-2.1 5.3-5 6.5-2.9-1.2-5-3.4-5-6.5v-4z"sv;
static constexpr auto NEW_TAB_ICON_PATH = "M8 3v10M3 8h10"sv;
static constexpr auto CLOSE_ICON_PATH = "M4.5 4.5l7 7M11.5 4.5l-7 7"sv;
static constexpr auto TAB_CLOSE_ICON_PATH = "M4 4l8 8M12 4l-8 8"sv;
static constexpr auto CHEVRON_DOWN_ICON_PATH = "M4.5 6.5L8 10l3.5-3.5"sv;
static constexpr auto CHEVRON_UP_ICON_PATH = "M4.5 9.5L8 6l3.5 3.5"sv;
static constexpr auto SIDEBAR_ICON_PATH = "M4 3h8a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM6 3v10"sv;
static constexpr auto DOWNLOAD_ICON_PATH = "M8 3v6.5M5 6.8L8 9.8l3-3M3.5 12.5h9"sv;
static constexpr auto AUDIO_ICON_PATH = "M2.5 6h2.5L8.5 3v10L5 10H2.5zM11 5.5a3.5 3.5 0 0 1 0 5"sv;
static constexpr auto MUTED_ICON_PATH = "M2.5 6h2.5L8.5 3v10L5 10H2.5zM11 6l3.5 4M14.5 6L11 10"sv;
static constexpr auto PIN_ICON_PATH = "M8 10v4M5 10h6M6.2 2.5h3.6l-.4 4 1.6 3.5H5l1.6-3.5z"sv;
static constexpr auto WARNING_ICON_PATH = "M8 2.5l6 11H2zM8 6.8v3M8 11.7v.1"sv;
static constexpr auto SEARCH_ICON_PATH = "M7 2.5a4.5 4.5 0 1 1 0 9a4.5 4.5 0 1 1 0-9zM10.5 10.5L14 14"sv;
static constexpr auto ZOOM_OUT_ICON_PATH = "M3 8h10"sv;
static constexpr auto PAUSE_ICON_PATH = "M6 4v8M10 4v8"sv;
static constexpr auto MENU_ICON_PATH = "M3 4.5h10M3 8h10M3 11.5h10"sv;
static constexpr auto GLOBE_ICON_PATH = "M8 2.5a5.5 5.5 0 1 1 0 11a5.5 5.5 0 1 1 0-11zM2.5 8h11M8 2.5c-1.6 1.6-2.4 3.4-2.4 5.5s.8 3.9 2.4 5.5M8 2.5c1.6 1.6 2.4 3.4 2.4 5.5s-.8 3.9-2.4 5.5"sv;
static constexpr auto DOCUMENT_ICON_PATH = "M3.5 2.5h6l3 3v8h-9zM9.5 2.5v3h3"sv;
static constexpr auto NETWORK_ICON_PATH = "M2.5 8a5.5 5.5 0 1 0 11 0a5.5 5.5 0 1 0-11 0M2.5 8h11M8 2.5c1.6 1.6 2.4 3.4 2.4 5.5S9.6 11.9 8 13.5M8 2.5C6.4 4.1 5.6 5.9 5.6 8s.8 3.9 2.4 5.5"sv;
static constexpr auto COMPOSITOR_ICON_PATH = "M2.5 4h7v5h-7zM6.5 7h7v5h-7z"sv;
static constexpr auto IMAGE_ICON_PATH = "M2.5 3.5h11v9h-11zM2.5 10.5l3-3 3 3 2-2 3 3M10.5 6.2v.1"sv;
static constexpr auto FOLDER_ICON_PATH = "M2.5 4.5a1 1 0 0 1 1-1h3l1.5 1.5h4.5a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1z"sv;

// The window controls of Windows and Linux are drawn on a 10 px box in the middle of the icon.
static constexpr auto WINDOW_MINIMIZE_ICON_PATH = "M3 8.5h10"sv;
static constexpr auto WINDOW_MAXIMIZE_ICON_PATH = "M5 3.5h6a1.5 1.5 0 0 1 1.5 1.5v6a1.5 1.5 0 0 1-1.5 1.5H5A1.5 1.5 0 0 1 3.5 11V5A1.5 1.5 0 0 1 5 3.5z"sv;
static constexpr auto WINDOW_RESTORE_ICON_PATH = "M4.5 6h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1zM6 3.5h5.5a1 1 0 0 1 1 1V10"sv;
static constexpr auto WINDOW_CLOSE_ICON_PATH = "M3.5 3.5l9 9M12.5 3.5l-9 9"sv;

static constexpr qreal CHROME_ICON_STROKE_WIDTH = 1.5;
static constexpr qreal TOOLBAR_ICON_STROKE_WIDTH = 1.4;

static StringView chrome_icon_path(ChromeIcon icon)
{
    switch (icon) {
    case ChromeIcon::Back:
        return BACK_ICON_PATH;
    case ChromeIcon::Forward:
        return FORWARD_ICON_PATH;
    case ChromeIcon::Reload:
        return RELOAD_ICON_PATH;
    case ChromeIcon::Stop:
        return STOP_ICON_PATH;
    case ChromeIcon::NewTab:
        return NEW_TAB_ICON_PATH;
    case ChromeIcon::Close:
        return CLOSE_ICON_PATH;
    case ChromeIcon::TabClose:
        return TAB_CLOSE_ICON_PATH;
    case ChromeIcon::Menu:
        return MENU_ICON_PATH;
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
        return NETWORK_ICON_PATH;
    case ChromeIcon::Compositor:
        return COMPOSITOR_ICON_PATH;
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
    case ChromeIcon::ContentBlocking:
        return CONTENT_BLOCKING_ICON_PATH;
    case ChromeIcon::SiteSettings:
        return SITE_SETTINGS_ICON_PATH;
    case ChromeIcon::Microphone:
        return MICROPHONE_ICON_PATH;
    case ChromeIcon::Location:
        return LOCATION_ICON_PATH;
    case ChromeIcon::SelectArea:
        return SELECT_AREA_ICON_PATH;
    case ChromeIcon::CheckCircle:
        return CHECK_CIRCLE_ICON_PATH;
    case ChromeIcon::Pin:
        return PIN_ICON_PATH;
    case ChromeIcon::Warning:
        return WARNING_ICON_PATH;
    case ChromeIcon::Pause:
        return PAUSE_ICON_PATH;
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

// On a 1x screen a 1.5 px line cannot land on whole pixels and smears, so the small icons of the chrome have a second
// drawing for it: 1 px lines on half pixels, so that their straight edges are crisp, and a 1.25 px line for the diagonals
// of a cross. The drawings are on the same 16 px canvas.
struct PixelSnappedChromeIcon {
    StringView path;
    qreal stroke_width { 1 };
};

// The 1x drawings are used for icons shown within this much of the canvas's size.
static constexpr int PIXEL_SNAPPED_ICON_SIZE_TOLERANCE = 2;

static Optional<PixelSnappedChromeIcon> pixel_snapped_chrome_icon(ChromeIcon icon)
{
    switch (icon) {
    case ChromeIcon::Close:
    case ChromeIcon::TabClose:
        return PixelSnappedChromeIcon { "M4 4L12 12M12 4L4 12"sv, 1.25 };
    case ChromeIcon::NewTab:
        return PixelSnappedChromeIcon { "M7.5 2.5v10M2.5 7.5h10"sv };
    case ChromeIcon::Volume:
        return PixelSnappedChromeIcon { "M2.5 5.5H5l3.5-3v11L5 10.5H2.5zM10.9 5.3Q12.6 6.4 12.6 8T10.9 10.7"sv };
    case ChromeIcon::VolumeMuted:
        return PixelSnappedChromeIcon { "M2.5 5.5H5l3.5-3v11L5 10.5H2.5zM10 6l4 4M14 6l-4 4"sv };
    case ChromeIcon::Download:
        return PixelSnappedChromeIcon { "M7.5 2v7.5M4.5 6.5l3 3 3-3M2.5 12.5h10"sv };
    default:
        return {};
    }
}

static QPainterPath const* chrome_icon_pixel_snapped_painter_path(ChromeIcon icon)
{
    static HashMap<ChromeIcon, Optional<QPainterPath>> s_paths;
    auto const& path = s_paths.ensure(icon, [&]() -> Optional<QPainterPath> {
        auto snapped = pixel_snapped_chrome_icon(icon);
        if (!snapped.has_value())
            return {};
        return mirrored_for_right_sidebar(icon, parse_svg_path(snapped->path));
    });
    return path.has_value() ? &path.value() : nullptr;
}

static QPixmap create_transparent_icon_pixmap(QSize logical_size, qreal device_pixel_ratio)
{
    QPixmap pixmap(physical_size_for_device_pixel_ratio(logical_size, device_pixel_ratio));
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    pixmap.fill(Qt::transparent);
    return pixmap;
}

// The icon is drawn from its vector at the size and device pixel ratio it is shown at, never scaled from a picture of it.
// NB: The size the icon was made for sets its line: the toolbar's larger icons are drawn with a finer line on their
//     canvas, so that they keep about the weight of the smaller ones.
static QPixmap create_chrome_icon_pixmap(ChromeIcon icon, QPainterPath const& path, QColor color, qreal device_pixel_ratio, int logical_size, int design_size)
{
    auto pixmap = create_transparent_icon_pixmap({ logical_size, logical_size }, device_pixel_ratio);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // An icon is drawn in one opaque color, then made as translucent as its color as a whole, so that where its lines
    // cross they don't add up to a brighter spot.
    auto opaque_color = color;
    opaque_color.setAlpha(255);

    // NB: The 1x drawing keeps the size of the 16 px canvas, so it is only used for icons shown at about that size. A
    //     smaller icon, such as a badge's, is scaled from its vector.
    auto has_pixel_snapped_size = device_pixel_ratio == 1 && abs(logical_size - CHROME_ICON_CANVAS_SIZE) <= PIXEL_SNAPPED_ICON_SIZE_TOLERANCE;
    auto const* pixel_snapped_path = has_pixel_snapped_size ? chrome_icon_pixel_snapped_painter_path(icon) : nullptr;
    if (pixel_snapped_path) {
        // NB: The 1x drawing keeps its own pixels, so it is centered on whole pixels rather than scaled.
        auto offset = (logical_size - CHROME_ICON_CANVAS_SIZE) / 2;
        painter.translate(offset, offset);
        painter.setPen(QPen(opaque_color, pixel_snapped_chrome_icon(icon)->stroke_width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(*pixel_snapped_path);
    } else {
        painter.scale(static_cast<qreal>(logical_size) / CHROME_ICON_CANVAS_SIZE, static_cast<qreal>(logical_size) / CHROME_ICON_CANVAS_SIZE);

        auto stroke_width = CHROME_ICON_STROKE_WIDTH;
        // NB: The window controls of Windows are drawn with hairlines, as the system draws its own.
        if (icon == ChromeIcon::WindowMinimize || icon == ChromeIcon::WindowMaximize || icon == ChromeIcon::WindowRestore || icon == ChromeIcon::WindowClose)
            stroke_width = 1.0;
        else if (design_size == TOOLBAR_ICON_SIZE)
            stroke_width = TOOLBAR_ICON_STROKE_WIDTH;

        if (icon == ChromeIcon::Sidebar || icon == ChromeIcon::SidebarRight) {
            // Fill the combined outline once so antialiased edges do not accumulate where the divider meets the frame.
            QPainterPathStroker stroker;
            stroker.setWidth(stroke_width);
            stroker.setCapStyle(Qt::RoundCap);
            stroker.setJoinStyle(Qt::RoundJoin);
            auto stroke = stroker.createStroke(path);
            stroke.setFillRule(Qt::WindingFill);
            painter.fillPath(stroke.simplified(), opaque_color);
        } else {
            painter.setPen(QPen(opaque_color, stroke_width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(icon == ChromeIcon::StarFilled ? QBrush(opaque_color) : QBrush(Qt::NoBrush));
            painter.drawPath(path);
        }
    }

    if (color.alpha() != 255) {
        painter.resetTransform();
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(QRect(QPoint(), pixmap.size()), QColor(0, 0, 0, color.alpha()));
    }

    return pixmap;
}

// Draws a chrome icon from its vector for each size and device pixel ratio it is asked for.
class ChromeIconEngine final : public QIconEngine {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct Colors {
        QColor normal;
        QColor active;
        QColor disabled;
        QColor selected;
    };

    ChromeIconEngine(ChromeIcon icon, QPainterPath path, Colors colors, int design_size)
        : m_icon(icon)
        , m_path(AK::move(path))
        , m_colors(AK::move(colors))
        , m_design_size(design_size)
    {
    }

    virtual QIconEngine* clone() const override { return new ChromeIconEngine(m_icon, m_path, m_colors, m_design_size); }

    virtual QSize actualSize(QSize const& size, QIcon::Mode, QIcon::State) override
    {
        auto side = min(size.width(), size.height());
        return { side, side };
    }

    virtual QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return { QSize(m_design_size, m_design_size) }; }

    virtual QPixmap pixmap(QSize const& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1);
    }

    virtual QPixmap scaledPixmap(QSize const& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        auto side = min(size.width(), size.height());
        if (side <= 0)
            return {};
        return create_chrome_icon_pixmap(m_icon, m_path, color_for_mode(mode), scale, side, m_design_size);
    }

    virtual void paint(QPainter* painter, QRect const& rect, QIcon::Mode mode, QIcon::State state) override
    {
        auto side = min(rect.width(), rect.height());
        auto device_pixel_ratio = painter->device() ? painter->device()->devicePixelRatio() : 1;
        auto pixmap = scaledPixmap(QSize(side, side), mode, state, device_pixel_ratio);
        auto position = rect.topLeft() + QPoint((rect.width() - side) / 2, (rect.height() - side) / 2);
        painter->drawPixmap(position, pixmap);
    }

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

    ChromeIcon m_icon;
    QPainterPath m_path;
    Colors m_colors;
    int m_design_size { CHROME_ICON_CANVAS_SIZE };
};

QPixmap create_chrome_icon_pixmap(ChromeIcon icon, QColor color, qreal device_pixel_ratio)
{
    return create_chrome_icon_pixmap(icon, chrome_icon_painter_path(icon), color, device_pixel_ratio, CHROME_ICON_CANVAS_SIZE, CHROME_ICON_CANVAS_SIZE);
}

static QIcon create_chrome_icon(ChromeIcon icon, QPainterPath const& path, QPalette const& palette, ChromeStyle::WindowActivity activity, int logical_size)
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

    return QIcon(new ChromeIconEngine(icon, path, { normal, chrome.icon, chrome.disabled, chrome.icon }, logical_size));
}

QIcon create_chrome_icon(ChromeIcon icon, QPalette const& palette, ChromeStyle::WindowActivity activity, int logical_size)
{
    return create_chrome_icon(icon, chrome_icon_painter_path(icon), palette, activity, logical_size);
}

// The icon is two lines, each with a knob on it, the top one to the right and the bottom one to the left. The knobs
// slide past each other as the travel goes from 0 to 1, and each line breaks around its knob.
QIcon create_site_settings_icon(QPalette const& palette, qreal knob_travel)
{
    static constexpr qreal LINE_START = 2.5;
    static constexpr qreal LINE_END = 13.5;
    static constexpr qreal KNOB_RADIUS = 1.6;
    static constexpr qreal KNOB_GAP = 2;
    static constexpr qreal KNOB_TRAVEL = 5;

    QPainterPath path;
    auto add_line_with_knob = [&](qreal y, qreal knob_x) {
        if (knob_x - KNOB_GAP > LINE_START) {
            path.moveTo(LINE_START, y);
            path.lineTo(knob_x - KNOB_GAP, y);
        }
        if (knob_x + KNOB_GAP < LINE_END) {
            path.moveTo(knob_x + KNOB_GAP, y);
            path.lineTo(LINE_END, y);
        }
        path.addEllipse(QPointF(knob_x, y), KNOB_RADIUS, KNOB_RADIUS);
    };
    add_line_with_knob(5, 10.5 - (KNOB_TRAVEL * knob_travel));
    add_line_with_knob(11, 5.5 + (KNOB_TRAVEL * knob_travel));

    return create_chrome_icon(ChromeIcon::SiteSettings, path, palette, ChromeStyle::WindowActivity::Active, CHROME_ICON_CANVAS_SIZE);
}

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
