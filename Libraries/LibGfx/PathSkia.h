/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Path.h>

class SkPath;

namespace Gfx {

class PathImplSkia final : public PathImpl {
public:
    static NonnullOwnPtr<Gfx::PathImplSkia> create();

    virtual ~PathImplSkia() override;

    virtual void clear() override;
    virtual void move_to(Gfx::FloatPoint const&) override;
    virtual void line_to(Gfx::FloatPoint const&) override;
    virtual void close_all_subpaths() override;
    virtual void close() override;
    virtual void elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep) override;
    virtual void arc_to(FloatPoint point, float radius, bool large_arc, bool sweep) override;
    virtual void quadratic_bezier_curve_to(FloatPoint through, FloatPoint point) override;
    virtual void cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2) override;
    virtual void text(Utf8View, Font const&) override;

    virtual void append_path(Gfx::Path const&) override;
    virtual void intersect(Gfx::Path const&) override;

    [[nodiscard]] virtual bool is_empty() const override;
    virtual Gfx::FloatPoint last_point() const override;
    virtual Gfx::FloatRect bounding_box() const override;
    virtual bool contains(FloatPoint point, Gfx::WindingRule) const override;
    virtual void set_fill_type(Gfx::WindingRule winding_rule) override;

    virtual NonnullOwnPtr<PathImpl> clone() const override;
    virtual NonnullOwnPtr<PathImpl> copy_transformed(Gfx::AffineTransform const&) const override;
    virtual NonnullOwnPtr<PathImpl> place_text_along(Utf8View text, Font const&) const override;

    SkPath const& sk_path() const { return *m_path; }
    SkPath& sk_path() { return *m_path; }

private:
    PathImplSkia();
    PathImplSkia(PathImplSkia const& other);

    Gfx::FloatPoint m_last_move_to;
    NonnullOwnPtr<SkPath> m_path;
};

}
