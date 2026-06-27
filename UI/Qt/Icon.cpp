/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibCore/Resource.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QTransform>

namespace Ladybird {

QIcon load_icon_from_uri(StringView uri)
{
    auto resource = MUST(Core::Resource::load_from_uri(uri));
    auto path = qstring_from_ak_string(resource->filesystem_path());

    return QIcon { path };
}

QIcon icon_from_base64_png(StringView favicon_base64_png, int logical_size)
{
    auto decoded = decode_base64(favicon_base64_png);
    if (decoded.is_error())
        return {};

    QPixmap pixmap;
    if (!pixmap.loadFromData(decoded.value().data(), static_cast<uint>(decoded.value().size()), "PNG"))
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

static QPen chrome_icon_pen(QColor const& color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

static void draw_stroked_icon_path(QPainter& painter, QPainterPath const& path, QColor const& color, qreal width)
{
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);

    auto stroke = stroker.createStroke(path);
    stroke.setFillRule(Qt::WindingFill);
    painter.fillPath(stroke, color);
}

static void draw_back_icon(QPainter& painter, QColor const& color)
{
    QPainterPath path;
    path.moveTo(17.2, 10.3);
    path.lineTo(3.8, 10.3);
    path.moveTo(9.7, 5.0);
    path.lineTo(3.8, 10.3);
    path.lineTo(9.7, 15.6);
    draw_stroked_icon_path(painter, path, color, 2.0);
}

static void draw_forward_icon(QPainter& painter, QColor const& color)
{
    QPainterPath path;
    path.moveTo(2.8, 10.3);
    path.lineTo(16.2, 10.3);
    path.moveTo(10.3, 5.0);
    path.lineTo(16.2, 10.3);
    path.lineTo(10.3, 15.6);
    draw_stroked_icon_path(painter, path, color, 2.0);
}

static void draw_reload_icon(QPainter& painter, QColor const& color)
{
    QPainterPath shape;
    shape.setFillRule(Qt::WindingFill);
    shape.arcMoveTo(QRectF(3.0, 2.5, 14.0, 14.0), 58.0);
    shape.arcTo(QRectF(3.0, 2.5, 14.0, 14.0), 58.0, 272.0);
    shape.arcTo(QRectF(4.9, 4.4, 10.2, 10.2), 330.0, -272.0);
    shape.closeSubpath();

    QPainterPath arrow_head;
    arrow_head.moveTo(17.1, 7.05);
    arrow_head.lineTo(14.65, 1.55);
    arrow_head.lineTo(11.35, 7.05);
    arrow_head.closeSubpath();
    shape.addPath(arrow_head);

    painter.fillPath(shape, color);
}

static void draw_star_icon(QPainter& painter, QColor const& color, bool filled)
{
    QPainterPath path;
    path.moveTo(10.0, 4.1);
    path.lineTo(12.5, 9.0);
    path.lineTo(18.0, 9.5);
    path.lineTo(13.9, 13.2);
    path.lineTo(15.1, 18.7);
    path.lineTo(10.0, 15.8);
    path.lineTo(4.9, 18.7);
    path.lineTo(6.1, 13.2);
    path.lineTo(2.0, 9.5);
    path.lineTo(7.5, 9.0);
    path.closeSubpath();

    painter.setPen(chrome_icon_pen(color, filled ? 1.2 : 1.65));
    if (filled)
        painter.setBrush(color);
    else
        painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

static QPainterPath horizontally_flipped_path(QPainterPath const& path)
{
    QTransform transform;
    transform.translate(20.0, 0.0);
    transform.scale(-1.0, 1.0);
    return transform.map(path);
}

static void draw_vertical_tab_bar_icon(QPainter& painter, QColor const& color, bool expanded, bool right_side)
{
    QPainterPath tab_bar;
    tab_bar.addRoundedRect(QRectF(3.8, 3.2, 12.4, 11.6), 1.6, 1.6);
    tab_bar.moveTo(7.8, 3.6);
    tab_bar.lineTo(7.8, 14.4);

    QPainterPath arrow;
    if (expanded) {
        arrow.moveTo(13.1, 6.8);
        arrow.lineTo(10.3, 9.0);
        arrow.lineTo(13.1, 11.2);
    } else {
        arrow.moveTo(10.7, 6.8);
        arrow.lineTo(13.5, 9.0);
        arrow.lineTo(10.7, 11.2);
    }

    if (right_side) {
        tab_bar = horizontally_flipped_path(tab_bar);
        arrow = horizontally_flipped_path(arrow);
    }

    draw_stroked_icon_path(painter, tab_bar, color, 1.55);
    draw_stroked_icon_path(painter, arrow, color, 1.85);
}

static void draw_globe_icon(QPainter& painter, QColor const& color)
{
    auto outline_rect = QRectF(3.5, 3.5, 13, 13);
    QRectF meridian_rect(7.5, outline_rect.top() + 0.25, 5.0, outline_rect.height() - 0.5);

    QPainterPath path;
    path.addEllipse(outline_rect);
    path.moveTo(QPointF(outline_rect.left() + 1.3, 10));
    path.lineTo(QPointF(outline_rect.right() - 1.3, 10));
    path.arcMoveTo(meridian_rect, 90);
    path.arcTo(meridian_rect, 90, 180);
    path.arcMoveTo(meridian_rect, 270);
    path.arcTo(meridian_rect, 270, 180);

    draw_stroked_icon_path(painter, path, color, 1.55);
}

static void draw_volume_icon(QPainter& painter, QColor const& color, bool muted)
{
    QPainterPath speaker;
    speaker.setFillRule(Qt::WindingFill);

    speaker.moveTo(0, 13);
    speaker.lineTo(4, 13);
    speaker.lineTo(10, 18);
    speaker.lineTo(10, 0);
    speaker.lineTo(4, 5);
    speaker.lineTo(0, 5);
    speaker.closeSubpath();
    painter.fillPath(speaker, color);

    if (muted) {
        painter.drawLine(QPoint { 0, 0 }, QPoint { 17, 18 });
        return;
    }

    QPainterPath inner_wave;
    inner_wave.moveTo(12, 5);
    inner_wave.lineTo(12, 13);
    inner_wave.cubicTo(13.6, 13, 14.9, 11.21, 14.9, 9);
    inner_wave.cubicTo(14.9, 6.79, 13.6, 5, 12, 5);
    inner_wave.closeSubpath();
    painter.fillPath(inner_wave, color);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(chrome_icon_pen(color, 1.6));
    painter.drawArc(QRectF(7.3, 1.4, 10.8, 15.2), 90 * 16, -180 * 16);
}

static QPixmap create_transparent_icon_pixmap(QSize logical_size, qreal device_pixel_ratio)
{
    QPixmap pixmap(physical_size_for_device_pixel_ratio(logical_size, device_pixel_ratio));
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    pixmap.fill(Qt::transparent);
    return pixmap;
}

static QPixmap create_chrome_icon_pixmap(ChromeIcon icon, QColor color, qreal device_pixel_ratio)
{
    auto pixmap = create_transparent_icon_pixmap({ 20, 20 }, device_pixel_ratio);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(chrome_icon_pen(color, 1.7));

    switch (icon) {
    case ChromeIcon::Back:
        draw_back_icon(painter, color);
        break;
    case ChromeIcon::Forward:
        draw_forward_icon(painter, color);
        break;
    case ChromeIcon::Reload:
        draw_reload_icon(painter, color);
        break;
    case ChromeIcon::Stop:
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawLine(QPointF(6.0, 6.0), QPointF(14.0, 14.0));
        painter.drawLine(QPointF(14.0, 6.0), QPointF(6.0, 14.0));
        break;
    case ChromeIcon::NewTab:
        painter.setPen(chrome_icon_pen(color, 1.9));
        painter.drawLine(QPointF(10.0, 4.3), QPointF(10.0, 15.7));
        painter.drawLine(QPointF(4.3, 10.0), QPointF(15.7, 10.0));
        break;
    case ChromeIcon::Close:
        painter.setPen(chrome_icon_pen(color, 1.9));
        painter.drawLine(QPointF(5.5, 7.15), QPointF(14.5, 16.65));
        painter.drawLine(QPointF(14.5, 7.15), QPointF(5.5, 16.65));
        break;
    case ChromeIcon::TabClose:
        painter.setPen(chrome_icon_pen(color, 1.9));
        painter.drawLine(QPointF(5.5, 5.65), QPointF(14.5, 15.15));
        painter.drawLine(QPointF(14.5, 5.65), QPointF(5.5, 15.15));
        break;
    case ChromeIcon::Menu:
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawLine(QPointF(4.1, 6.2), QPointF(15.9, 6.2));
        painter.drawLine(QPointF(4.1, 10.0), QPointF(15.9, 10.0));
        painter.drawLine(QPointF(4.1, 13.8), QPointF(15.9, 13.8));
        break;
    case ChromeIcon::Star:
        draw_star_icon(painter, color, false);
        break;
    case ChromeIcon::StarFilled:
        draw_star_icon(painter, color, true);
        break;
    case ChromeIcon::Search:
        painter.setPen(chrome_icon_pen(color, 1.55));
        painter.drawEllipse(QRectF(4.2, 4.0, 9.7, 9.7));
        painter.drawLine(QPointF(12.1, 12.1), QPointF(16.0, 16.0));
        break;
    case ChromeIcon::Globe:
        draw_globe_icon(painter, color);
        break;
    case ChromeIcon::Folder: {
        painter.setPen(chrome_icon_pen(color, 1.6));
        QPainterPath path;
        path.moveTo(2.8, 6.1);
        path.lineTo(7.6, 6.1);
        path.lineTo(9.2, 8.0);
        path.lineTo(17.2, 8.0);
        path.quadTo(18.0, 8.0, 18.0, 8.8);
        path.lineTo(18.0, 15.1);
        path.quadTo(18.0, 15.9, 17.2, 15.9);
        path.lineTo(2.8, 15.9);
        path.quadTo(2.0, 15.9, 2.0, 15.1);
        path.lineTo(2.0, 6.9);
        path.quadTo(2.0, 6.1, 2.8, 6.1);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case ChromeIcon::Download:
    case ChromeIcon::DownloadActive: {
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawLine(QPointF(10.0, 3.4), QPointF(10.0, 12.0));
        painter.drawLine(QPointF(6.6, 8.7), QPointF(10.0, 12.1));
        painter.drawLine(QPointF(13.4, 8.7), QPointF(10.0, 12.1));

        QPainterPath tray;
        tray.moveTo(4.0, 13.4);
        tray.lineTo(4.0, 15.7);
        tray.quadTo(4.0, 16.6, 4.9, 16.6);
        tray.lineTo(15.1, 16.6);
        tray.quadTo(16.0, 16.6, 16.0, 15.7);
        tray.lineTo(16.0, 13.4);
        painter.drawPath(tray);
        break;
    }
    case ChromeIcon::Volume:
        draw_volume_icon(painter, color, false);
        break;
    case ChromeIcon::VolumeMuted:
        draw_volume_icon(painter, color, true);
        break;
    case ChromeIcon::ChevronUp:
        painter.setPen(chrome_icon_pen(color, 1.85));
        painter.drawLine(QPointF(5.0, 12.4), QPointF(10.0, 7.4));
        painter.drawLine(QPointF(10.0, 7.4), QPointF(15.0, 12.4));
        break;
    case ChromeIcon::ChevronDown:
        painter.setPen(chrome_icon_pen(color, 1.85));
        painter.drawLine(QPointF(5.0, 7.6), QPointF(10.0, 12.6));
        painter.drawLine(QPointF(10.0, 12.6), QPointF(15.0, 7.6));
        break;
    case ChromeIcon::VerticalTabBarCollapse:
        draw_vertical_tab_bar_icon(painter, color, true, false);
        break;
    case ChromeIcon::VerticalTabBarCollapseRight:
        draw_vertical_tab_bar_icon(painter, color, true, true);
        break;
    case ChromeIcon::VerticalTabBarExpand:
        draw_vertical_tab_bar_icon(painter, color, false, false);
        break;
    case ChromeIcon::VerticalTabBarExpandRight:
        draw_vertical_tab_bar_icon(painter, color, false, true);
        break;
    case ChromeIcon::WindowMinimize:
        painter.setPen(chrome_icon_pen(color, 1.65));
        painter.drawLine(QPointF(5.2, 12.5), QPointF(14.8, 12.5));
        break;
    case ChromeIcon::WindowMaximize:
        painter.setPen(chrome_icon_pen(color, 1.55));
        painter.drawRoundedRect(QRectF(5.0, 5.0, 10.0, 10.0), 1.4, 1.4);
        break;
    case ChromeIcon::WindowRestore:
        painter.setPen(chrome_icon_pen(color, 1.55));
        painter.drawRoundedRect(QRectF(5.1, 7.0, 7.9, 7.9), 1.2, 1.2);
        painter.drawLine(QPointF(7.2, 5.1), QPointF(14.9, 5.1));
        painter.drawLine(QPointF(14.9, 5.1), QPointF(14.9, 12.9));
        break;
    case ChromeIcon::WindowClose:
        painter.setPen(chrome_icon_pen(color, 1.65));
        painter.drawLine(QPointF(5.5, 5.5), QPointF(14.5, 14.5));
        painter.drawLine(QPointF(14.5, 5.5), QPointF(5.5, 14.5));
        break;
    }

    return pixmap;
}

static QIcon create_y_offset_icon(QIcon const& source, int y_offset)
{
    constexpr int icon_size = 20;

    auto create_pixmap = [&](QIcon::Mode mode, int device_pixel_ratio) {
        auto pixmap = create_transparent_icon_pixmap({ icon_size, icon_size }, device_pixel_ratio);

        QPainter painter(&pixmap);
        painter.drawPixmap(
            QRect(0, y_offset, icon_size, icon_size),
            source.pixmap({ icon_size, icon_size }, static_cast<qreal>(device_pixel_ratio), mode));
        return pixmap;
    };

    QIcon icon;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS) {
        icon.addPixmap(create_pixmap(QIcon::Normal, device_pixel_ratio), QIcon::Normal);
        icon.addPixmap(create_pixmap(QIcon::Active, device_pixel_ratio), QIcon::Active);
        icon.addPixmap(create_pixmap(QIcon::Disabled, device_pixel_ratio), QIcon::Disabled);
        icon.addPixmap(create_pixmap(QIcon::Selected, device_pixel_ratio), QIcon::Selected);
    }
    return icon;
}

QIcon create_chrome_icon(ChromeIcon icon, QPalette const& palette)
{
    auto normal = ChromeStyle::chrome_button_text(palette);
    auto normal_alpha = 216;
    if (icon == ChromeIcon::DownloadActive) {
        normal = ChromeStyle::chrome_accent(palette);
        normal_alpha = 244;
    } else if (icon == ChromeIcon::Close || icon == ChromeIcon::TabClose)
        normal_alpha = 172;
    else if (icon == ChromeIcon::Star)
        normal_alpha = 204;
    else if (icon == ChromeIcon::Globe || icon == ChromeIcon::Search || icon == ChromeIcon::Menu)
        normal_alpha = 188;
    normal.setAlpha(normal_alpha);

    auto active = ChromeStyle::chrome_button_text(palette);
    active.setAlpha(icon == ChromeIcon::Close || icon == ChromeIcon::TabClose ? 220 : 236);

    auto disabled = ChromeStyle::chrome_muted_text(palette);
    disabled.setAlpha(icon == ChromeIcon::Close || icon == ChromeIcon::TabClose ? 78 : 96);

    QIcon qicon;

    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS) {
        qicon.addPixmap(create_chrome_icon_pixmap(icon, normal, device_pixel_ratio), QIcon::Normal);
        qicon.addPixmap(create_chrome_icon_pixmap(icon, active, device_pixel_ratio), QIcon::Active);
        qicon.addPixmap(create_chrome_icon_pixmap(icon, disabled, device_pixel_ratio), QIcon::Disabled);
        qicon.addPixmap(create_chrome_icon_pixmap(icon, active, device_pixel_ratio), QIcon::Selected);
    }

    if (icon == ChromeIcon::Back || icon == ChromeIcon::Forward || icon == ChromeIcon::NewTab)
        return create_y_offset_icon(qicon, -1);
    return qicon;
}

static QPixmap create_loading_spinner_icon_pixmap(QPalette const& palette, int frame, int device_pixel_ratio)
{
    static constexpr int icon_size = 16;
    static constexpr int segment_count = 12;

    auto pixmap = create_transparent_icon_pixmap({ icon_size, icon_size }, device_pixel_ratio);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(icon_size / 2.0, icon_size / 2.0);

    auto color = ChromeStyle::chrome_text(palette);
    for (int segment = 0; segment < segment_count; ++segment) {
        auto segment_color = color;
        segment_color.setAlpha(((segment - frame + segment_count) % segment_count + 1) * 255 / segment_count);

        painter.save();
        painter.rotate(segment * 360.0 / segment_count);
        painter.setPen(QPen(segment_color, 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(0, -4), QPointF(0, -7));
        painter.restore();
    }

    return pixmap;
}

QIcon loading_spinner_icon(QPalette const& palette, int frame)
{
    QIcon icon;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS)
        icon.addPixmap(create_loading_spinner_icon_pixmap(palette, frame, device_pixel_ratio));
    return icon;
}

QSize physical_size_for_device_pixel_ratio(QSize size, qreal device_pixel_ratio)
{
    return {
        static_cast<int>(ceil(size.width() * device_pixel_ratio)),
        static_cast<int>(ceil(size.height() * device_pixel_ratio)),
    };
}

}
