/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Icon.h"
#include "StringUtils.h"
#include "TVGIconEngine.h"
#include <LibCore/Resource.h>
#include <QPalette>

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
        return [color = Color::from_argb(color.rgba64().toArgb32())](Gfx::Color icon_color) {
            return color.with_alpha((icon_color.alpha() * color.alpha()) / 255);
        };
    };
    icon_engine->add_filter(QIcon::Mode::Normal, icon_filter(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText)));
    icon_engine->add_filter(QIcon::Mode::Disabled, icon_filter(palette.color(QPalette::ColorGroup::Disabled, QPalette::ColorRole::ButtonText)));
    icon_engine->add_filter(QIcon::Mode::Active, icon_filter(palette.color(QPalette::ColorGroup::Active, QPalette::ColorRole::ButtonText)));
    icon_engine->add_filter(QIcon::Mode::Selected, icon_filter(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText)));

    return QIcon(icon_engine);
}

}
