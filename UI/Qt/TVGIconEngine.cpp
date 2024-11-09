/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <LibGfx/DeprecatedPainter.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TVGIconEngine.h>

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>

namespace Ladybird {

void TVGIconEngine::paint(QPainter* qpainter, QRect const& rect, QIcon::Mode mode, QIcon::State state)
{
    qpainter->drawPixmap(rect, pixmap(rect.size(), mode, state));
}

QIconEngine* TVGIconEngine::clone() const
{
    return new TVGIconEngine(*this);
}

QPixmap TVGIconEngine::pixmap(QSize const& size, QIcon::Mode mode, QIcon::State state)
{
    QPixmap pixmap;
    auto key = pixmap_cache_key(size, mode, state);
    if (QPixmapCache::find(key, &pixmap))
        return pixmap;
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { size.width(), size.height() }));
    Gfx::DeprecatedPainter painter { *bitmap };
    m_image_data->draw_into(painter, bitmap->rect());
    for (auto const& filter : m_filters) {
        if (filter->mode() == mode) {
            painter.blit_filtered({}, *bitmap, bitmap->rect(), filter->function(), false);
            break;
        }
    }
    QImage qimage { bitmap->scanline_u8(0), bitmap->width(), bitmap->height(), QImage::Format::Format_ARGB32 };
    pixmap = QPixmap::fromImage(qimage);
    if (!pixmap.isNull())
        QPixmapCache::insert(key, pixmap);
    return pixmap;
}

QString TVGIconEngine::pixmap_cache_key(QSize const& size, QIcon::Mode mode, QIcon::State state)
{
    return qstring_from_ak_string(
        MUST(String::formatted("$sernity_tvgicon_{}_{}x{}_{}_{}", m_cache_id, size.width(), size.height(), to_underlying(mode), to_underlying(state))));
}

void TVGIconEngine::add_filter(QIcon::Mode mode, Function<Color(Color)> filter)
{
    m_filters.empend(adopt_ref(*new Filter(mode, move(filter))));
    invalidate_cache();
}

TVGIconEngine* TVGIconEngine::from_file(QString const& path)
{
    QFile icon_resource(path);
    if (!icon_resource.open(QIODeviceBase::ReadOnly))
        return nullptr;
    auto icon_data = icon_resource.readAll();
    FixedMemoryStream icon_bytes { ReadonlyBytes { icon_data.data(), static_cast<size_t>(icon_data.size()) } };
    if (auto tvg = Gfx::TinyVGDecodedImageData::decode(icon_bytes); !tvg.is_error())
        return new TVGIconEngine(tvg.release_value());
    return nullptr;
}

}
