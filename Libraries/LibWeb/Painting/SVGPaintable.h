/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/AntiAliasing.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class SVGPaintable : public Paintable {
public:
    virtual StringView class_name() const override { return "SVGPaintable"sv; }

    Layout::Box const& layout_box() const;
    virtual Optional<CSSPixelRect> clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const;
    bool contributes_to_clip_path() const;

    virtual Optional<Gfx::AffineTransform> svg_viewport_transform() const override { return m_svg_viewport_transform; }
    virtual void set_svg_viewport_transform(Gfx::AffineTransform transform) override { m_svg_viewport_transform = transform; }

    virtual void reset_for_relayout() override
    {
        Paintable::reset_for_relayout();
        m_svg_viewport_transform = {};
    }

protected:
    virtual bool is_svg_paintable() const override { return true; }

    SVGPaintable(Layout::Box const&);

    virtual CSSPixelRect compute_absolute_rect() const override;

public:
    Gfx::ShouldAntiAlias should_anti_alias() const;

private:
    Optional<Gfx::AffineTransform> m_svg_viewport_transform;
};

template<>
inline bool Paintable::fast_is<SVGPaintable>() const { return is_svg_paintable(); }

}
