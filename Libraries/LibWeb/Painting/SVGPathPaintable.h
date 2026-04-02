/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Path.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>

namespace Web::Painting {

class WEB_API SVGPathPaintable final : public SVGGraphicsPaintable {
    GC_CELL(SVGPathPaintable, SVGGraphicsPaintable);
    GC_DECLARE_ALLOCATOR(SVGPathPaintable);

public:
    static GC::Ref<SVGPathPaintable> create(Layout::SVGGraphicsBox const&);

    virtual TraversalDecision hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const& callback) const override;
    virtual Optional<CSSPixelRect> clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const override;

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    SVG::SVGGraphicsElement const& dom_node() const { return as<SVG::SVGGraphicsElement>(*Paintable::dom_node()); }

    void set_computed_path(Gfx::Path path)
    {
        m_computed_path = move(path);
    }

    Optional<Gfx::Path> const& computed_path() const { return m_computed_path; }

    virtual void reset_for_relayout() override;

protected:
    SVGPathPaintable(Layout::SVGGraphicsBox const&);

    Optional<Gfx::Path> m_computed_path = {};

private:
    virtual bool is_svg_path_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGPathPaintable>() const { return is_svg_path_paintable(); }

}
