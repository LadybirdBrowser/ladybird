/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <LibGfx/Rect.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TVGIconEngine.h>

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>

namespace Ladybird {

void TVGIconEngine::paint(QPainter* qpainter, QRect const& rect, QIcon::Mode mode, QIcon::State state)
{
    auto device_pixel_ratio = qpainter->device() ? qpainter->device()->devicePixelRatioF() : 1.0;
    auto physical_size = physical_size_for_device_pixel_ratio(rect.size(), device_pixel_ratio);
    qpainter->drawPixmap(rect, render_pixmap(physical_size, device_pixel_ratio, mode, state));
}

QIconEngine* TVGIconEngine::clone() const
{
    return new TVGIconEngine(*this);
}

QPixmap TVGIconEngine::pixmap(QSize const& size, QIcon::Mode mode, QIcon::State state)
{
    return render_pixmap(size, 1.0, mode, state);
}

QPixmap TVGIconEngine::scaledPixmap(QSize const& size, QIcon::Mode mode, QIcon::State state, qreal scale)
{
    auto device_pixel_ratio = scale > 0.0 ? scale : 1.0;
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    auto physical_size = size;
#else
    auto physical_size = physical_size_for_device_pixel_ratio(size, device_pixel_ratio);
#endif
    return render_pixmap(physical_size, device_pixel_ratio, mode, state);
}

QPixmap TVGIconEngine::render_pixmap(QSize physical_size, qreal device_pixel_ratio, QIcon::Mode mode, QIcon::State state)
{
    QPixmap pixmap;
    auto key = pixmap_cache_key(physical_size, device_pixel_ratio, mode, state);
    if (QPixmapCache::find(key, &pixmap))
        return pixmap;
    auto bitmap = MUST(m_image_data->bitmap({ physical_size.width(), physical_size.height() }));

    for (auto const& filter : m_filters) {
        if (filter->mode() == mode) {
            for (int y = 0; y < bitmap->height(); ++y) {
                for (int x = 0; x < bitmap->width(); ++x) {
                    auto original_color = bitmap->get_pixel(x, y);
                    auto filtered_color = filter->function()(original_color);
                    bitmap->set_pixel(x, y, filtered_color);
                }
            }
            break;
        }
    }

    QImage qimage(
        bitmap->scanline_u8(0),
        bitmap->width(),
        bitmap->height(),
        static_cast<qsizetype>(bitmap->pitch()),
        QImage::Format::Format_ARGB32);

    pixmap = QPixmap::fromImage(qimage);
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    if (!pixmap.isNull())
        QPixmapCache::insert(key, pixmap);
    return pixmap;
}

QString TVGIconEngine::pixmap_cache_key(QSize physical_size, qreal device_pixel_ratio, QIcon::Mode mode, QIcon::State state)
{
    return qformatted("$sernity_tvgicon_{}_{}x{}_dpr{}_{}_{}",
        m_cache_id, physical_size.width(), physical_size.height(), static_cast<int>(device_pixel_ratio * 1000), to_underlying(mode), to_underlying(state));
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
