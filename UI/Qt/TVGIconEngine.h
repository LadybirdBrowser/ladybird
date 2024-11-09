/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGfx/ImageFormats/TinyVGLoader.h>

#include <QIconEngine>

namespace Ladybird {

class TVGIconEngine : public QIconEngine {
public:
    TVGIconEngine(Gfx::TinyVGDecodedImageData const& image_data)
        : m_image_data(image_data)
    {
    }

    static TVGIconEngine* from_file(QString const& path);

    void paint(QPainter* painter, QRect const& rect, QIcon::Mode mode,
        QIcon::State state) override;
    QIconEngine* clone() const override;
    QPixmap pixmap(QSize const& size, QIcon::Mode mode,
        QIcon::State state) override;

    void add_filter(QIcon::Mode mode, Function<Color(Color)> filter);

private:
    static unsigned next_cache_id()
    {
        static unsigned cache_id = 0;
        return cache_id++;
    }

    void invalidate_cache()
    {
        m_cache_id = next_cache_id();
    }

    class Filter : public RefCounted<Filter> {
    public:
        Filter(QIcon::Mode mode, Function<Color(Color)> function)
            : m_mode(mode)
            , m_function(move(function))
        {
        }
        QIcon::Mode mode() const { return m_mode; }
        Function<Color(Color)> const& function() const { return m_function; }

    private:
        QIcon::Mode m_mode;
        Function<Color(Color)> m_function;
    };

    QString pixmap_cache_key(QSize const& size, QIcon::Mode mode, QIcon::State state);

    Vector<NonnullRefPtr<Filter>> m_filters;
    NonnullRefPtr<Gfx::TinyVGDecodedImageData> m_image_data;
    unsigned m_cache_id { next_cache_id() };
};

}
