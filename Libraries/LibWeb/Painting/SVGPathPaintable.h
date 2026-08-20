/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Path.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::Painting {

class WEB_API SVGPathPaintable final : public SVGGraphicsPaintable {
public:
    static NonnullRefPtr<SVGPathPaintable> create(Layout::Box const&);

    virtual Optional<CSSPixelRect> clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const override;

    SVG::SVGGraphicsElement const& dom_node() const { return as<SVG::SVGGraphicsElement>(*Paintable::dom_node()); }

protected:
    SVGPathPaintable(Layout::Box const&);

private:
    virtual bool is_svg_path_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGPathPaintable>() const { return is_svg_path_paintable(); }

}
