/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class SVGSVGPaintable final : public Paintable {
public:
    static NonnullRefPtr<SVGSVGPaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "SVGSVGPaintable"sv; }

    void set_svg_viewport_size(CSSPixelSize viewport_size) { m_svg_viewport_size = viewport_size; }
    CSSPixelSize svg_viewport_size() const { return m_svg_viewport_size; }

    virtual Optional<Gfx::AffineTransform> svg_viewport_transform() const override { return m_svg_viewport_transform; }
    virtual void set_svg_viewport_transform(Gfx::AffineTransform transform) override { m_svg_viewport_transform = transform; }

    virtual void reset_for_relayout() override
    {
        Paintable::reset_for_relayout();
        m_svg_viewport_transform = {};
    }

protected:
    SVGSVGPaintable(Layout::Box const&);

private:
    virtual bool is_svg_svg_paintable() const final { return true; }

    CSSPixelSize m_svg_viewport_size;
    Optional<Gfx::AffineTransform> m_svg_viewport_transform;
};

template<>
inline bool Paintable::fast_is<SVGSVGPaintable>() const { return is_svg_svg_paintable(); }

}
