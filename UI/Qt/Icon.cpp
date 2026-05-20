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
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointF>

namespace Ladybird {

QIcon load_icon_from_uri(StringView uri)
{
    auto resource = MUST(Core::Resource::load_from_uri(uri));
    auto path = qstring_from_ak_string(resource->filesystem_path());

    return QIcon { path };
}

QIcon create_tvg_icon_with_theme_colors(QString const& name, QPalette const& palette)
{
    auto path = QString(":/Icons/%1.tvg").arg(name);

    auto* icon_engine = TVGIconEngine::from_file(path);
    VERIFY(icon_engine);

    auto icon_filter = [](QColor color) {
        return [color = Color::from_bgra(color.rgba64().toArgb32())](Gfx::Color icon_color) {
            return color.with_alpha((icon_color.alpha() * color.alpha()) / 255);
        };
    };
    icon_engine->add_filter(QIcon::Mode::Normal, icon_filter(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText)));
    icon_engine->add_filter(QIcon::Mode::Disabled, icon_filter(palette.color(QPalette::ColorGroup::Disabled, QPalette::ColorRole::ButtonText)));
    icon_engine->add_filter(QIcon::Mode::Active, icon_filter(palette.color(QPalette::ColorGroup::Active, QPalette::ColorRole::ButtonText)));
    icon_engine->add_filter(QIcon::Mode::Selected, icon_filter(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText)));

    return QIcon(icon_engine);
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
