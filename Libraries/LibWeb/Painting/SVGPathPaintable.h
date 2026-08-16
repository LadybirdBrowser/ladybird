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

namespace Web::Painting {

class WEB_API SVGPathPaintable final : public SVGGraphicsPaintable {
public:
    static NonnullRefPtr<SVGPathPaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "SVGPathPaintable"sv; }

    virtual Optional<CSSPixelRect> clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const override;

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void record_hit_test_items(DisplayListRecordingContext&, PaintPhase) const override;

    SVG::SVGGraphicsElement const& dom_node() const { return as<SVG::SVGGraphicsElement>(*Paintable::dom_node()); }

    // The identity is process-unique per layout-side path allocation and never
    // reused, so a match proves the already-held path is byte-identical and the
    // deep copy can be skipped. Every commit of a path-like fragment emits a
    // path (commit-side asserted), so the held path is never stale.
    void set_computed_path_if_identity_changed(Gfx::Path const& path, u64 path_identity)
    {
        if (path_identity != 0 && path_identity == m_committed_path_identity && m_computed_path.has_value())
            return;
        m_computed_path = path;
        m_committed_path_identity = path_identity;
    }

    Optional<Gfx::Path> const& computed_path() const { return m_computed_path; }

protected:
    SVGPathPaintable(Layout::Box const&);

    Optional<Gfx::Path> m_computed_path = {};
    u64 m_committed_path_identity { 0 };

private:
    virtual bool is_svg_path_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGPathPaintable>() const { return is_svg_path_paintable(); }

}
