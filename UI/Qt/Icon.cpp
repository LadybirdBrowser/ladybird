/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Resource.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TVGIconEngine.h>

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>

namespace Ladybird {

QIcon load_icon_from_uri(StringView uri)
{
    auto resource = MUST(Core::Resource::load_from_uri(uri));
    auto path = qstring_from_ak_string(resource->filesystem_path());

    return QIcon { path };
}

static QIcon create_tvg_icon_with_theme_colors(QString const& name, QPalette const& palette, int normal_alpha, int disabled_alpha, int active_alpha, int selected_alpha)
{
    auto path = QString(":/Icons/%1.tvg").arg(name);

    auto* icon_engine = TVGIconEngine::from_file(path);
    VERIFY(icon_engine);

    auto icon_filter = [](QColor color, int alpha) {
        color.setAlpha((color.alpha() * alpha) / 255);
        return [color = Color::from_bgra(color.rgba64().toArgb32())](Gfx::Color icon_color) {
            return color.with_alpha((icon_color.alpha() * color.alpha()) / 255);
        };
    };
    icon_engine->add_filter(QIcon::Mode::Normal, icon_filter(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText), normal_alpha));
    icon_engine->add_filter(QIcon::Mode::Disabled, icon_filter(palette.color(QPalette::ColorGroup::Disabled, QPalette::ColorRole::ButtonText), disabled_alpha));
    icon_engine->add_filter(QIcon::Mode::Active, icon_filter(palette.color(QPalette::ColorGroup::Active, QPalette::ColorRole::ButtonText), active_alpha));
    icon_engine->add_filter(QIcon::Mode::Selected, icon_filter(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText), selected_alpha));

    return QIcon(icon_engine);
}

QIcon create_tvg_icon_with_theme_colors(QString const& name, QPalette const& palette)
{
    return create_tvg_icon_with_theme_colors(name, palette, 255, 255, 255, 255);
}

static QPen chrome_icon_pen(QColor const& color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

static void draw_star_icon(QPainter& painter, QColor const& color, bool filled)
{
    QPainterPath path;
    path.moveTo(10.0, 2.6);
    path.lineTo(12.5, 7.5);
    path.lineTo(18.0, 8.0);
    path.lineTo(13.9, 11.7);
    path.lineTo(15.1, 17.2);
    path.lineTo(10.0, 14.3);
    path.lineTo(4.9, 17.2);
    path.lineTo(6.1, 11.7);
    path.lineTo(2.0, 8.0);
    path.lineTo(7.5, 7.5);
    path.closeSubpath();

    painter.setPen(chrome_icon_pen(color, filled ? 1.2 : 1.8));
    if (filled)
        painter.setBrush(color);
    else
        painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

static QPixmap create_chrome_icon_pixmap(ChromeIcon icon, QColor color)
{
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(chrome_icon_pen(color, 1.7));

    switch (icon) {
    case ChromeIcon::Back:
        painter.setPen(chrome_icon_pen(color, 2.05));
        painter.drawLine(QPointF(15.4, 10.0), QPointF(6.0, 10.0));
        painter.drawLine(QPointF(10.8, 5.5), QPointF(5.3, 10.0));
        painter.drawLine(QPointF(5.3, 10.0), QPointF(10.8, 14.5));
        break;
    case ChromeIcon::Forward:
        painter.setPen(chrome_icon_pen(color, 2.05));
        painter.drawLine(QPointF(4.6, 10.0), QPointF(14.0, 10.0));
        painter.drawLine(QPointF(9.4, 5.6), QPointF(14.7, 10.0));
        painter.drawLine(QPointF(14.7, 10.0), QPointF(9.4, 14.4));
        break;
    case ChromeIcon::Reload: {
        painter.setPen(chrome_icon_pen(color, 1.9));
        QPainterPath path;
        path.arcMoveTo(QRectF(4.0, 4.0, 12.2, 12.2), 34.0);
        path.arcTo(QRectF(4.0, 4.0, 12.2, 12.2), 34.0, 288.0);
        painter.drawPath(path);
        painter.drawLine(QPointF(15.4, 3.7), QPointF(16.0, 7.5));
        painter.drawLine(QPointF(15.4, 3.7), QPointF(11.8, 4.4));
        break;
    }
    case ChromeIcon::Stop:
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawLine(QPointF(6.0, 6.0), QPointF(14.0, 14.0));
        painter.drawLine(QPointF(14.0, 6.0), QPointF(6.0, 14.0));
        break;
    case ChromeIcon::NewTab:
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawLine(QPointF(10.0, 5.0), QPointF(10.0, 15.0));
        painter.drawLine(QPointF(5.0, 10.0), QPointF(15.0, 10.0));
        break;
    case ChromeIcon::Close:
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawLine(QPointF(6.0, 6.4), QPointF(14.0, 14.4));
        painter.drawLine(QPointF(14.0, 6.4), QPointF(6.0, 14.4));
        break;
    case ChromeIcon::Menu:
        painter.setPen(chrome_icon_pen(color, 1.8));
        painter.drawLine(QPointF(3.8, 6.4), QPointF(16.2, 6.4));
        painter.drawLine(QPointF(3.8, 10.0), QPointF(16.2, 10.0));
        painter.drawLine(QPointF(3.8, 13.6), QPointF(16.2, 13.6));
        break;
    case ChromeIcon::Star:
        draw_star_icon(painter, color, false);
        break;
    case ChromeIcon::StarFilled:
        draw_star_icon(painter, color, true);
        break;
    case ChromeIcon::Search:
        painter.setPen(chrome_icon_pen(color, 1.65));
        painter.drawEllipse(QRectF(4.2, 4.0, 9.7, 9.7));
        painter.drawLine(QPointF(12.1, 12.1), QPointF(16.0, 16.0));
        break;
    case ChromeIcon::Globe:
        painter.setPen(chrome_icon_pen(color, 1.75));
        painter.drawEllipse(QRectF(4.4, 4.4, 11.2, 11.2));
        painter.drawLine(QPointF(5.5, 10.0), QPointF(14.5, 10.0));
        painter.drawArc(QRectF(7.1, 4.4, 5.8, 11.2), 90 * 16, 180 * 16);
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

    auto create_pixmap = [&](QIcon::Mode mode) {
        QPixmap pixmap(icon_size, icon_size);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        source.paint(&painter, QRect(0, y_offset, icon_size, icon_size), Qt::AlignCenter, mode);
        return pixmap;
    };

    QIcon icon;
    icon.addPixmap(create_pixmap(QIcon::Normal), QIcon::Normal);
    icon.addPixmap(create_pixmap(QIcon::Active), QIcon::Active);
    icon.addPixmap(create_pixmap(QIcon::Disabled), QIcon::Disabled);
    icon.addPixmap(create_pixmap(QIcon::Selected), QIcon::Selected);
    return icon;
}

QIcon create_chrome_icon(ChromeIcon icon, QPalette const& palette)
{
    if (icon == ChromeIcon::Reload)
        return create_y_offset_icon(create_tvg_icon_with_theme_colors("reload", palette), 1);
    if (icon == ChromeIcon::Globe)
        return create_y_offset_icon(create_tvg_icon_with_theme_colors("globe", palette, 202, 96, 236, 236), 1);

    auto normal = palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText);
    auto normal_alpha = 216;
    if (icon == ChromeIcon::Close)
        normal_alpha = 172;
    else if (icon == ChromeIcon::Star)
        normal_alpha = 204;
    normal.setAlpha(normal_alpha);

    auto active = palette.color(QPalette::ColorGroup::Active, QPalette::ColorRole::ButtonText);
    active.setAlpha(icon == ChromeIcon::Close ? 220 : 236);

    auto disabled = palette.color(QPalette::ColorGroup::Disabled, QPalette::ColorRole::ButtonText);
    disabled.setAlpha(icon == ChromeIcon::Close ? 78 : 96);

    QIcon qicon;
    qicon.addPixmap(create_chrome_icon_pixmap(icon, normal), QIcon::Normal);
    qicon.addPixmap(create_chrome_icon_pixmap(icon, active), QIcon::Active);
    qicon.addPixmap(create_chrome_icon_pixmap(icon, disabled), QIcon::Disabled);
    qicon.addPixmap(create_chrome_icon_pixmap(icon, active), QIcon::Selected);
    return qicon;
}

QIcon loading_spinner_icon(QPalette const& palette, int frame)
{
    static constexpr int icon_size = 16;
    static constexpr int segment_count = 12;

    QPixmap pixmap(icon_size, icon_size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(icon_size / 2.0, icon_size / 2.0);

    auto color = palette.color(QPalette::Text);
    for (int segment = 0; segment < segment_count; ++segment) {
        auto segment_color = color;
        segment_color.setAlpha(((segment - frame + segment_count) % segment_count + 1) * 255 / segment_count);

        painter.save();
        painter.rotate(segment * 360.0 / segment_count);
        painter.setPen(QPen(segment_color, 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(0, -4), QPointF(0, -7));
        painter.restore();
    }

    return QIcon(pixmap);
}

}
