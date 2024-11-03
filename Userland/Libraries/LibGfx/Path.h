/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Utf8View.h>
#include <LibGfx/Forward.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

class PathImpl {
public:
    static NonnullOwnPtr<Gfx::PathImpl> create();

    virtual ~PathImpl();

    virtual void clear() = 0;
    virtual void move_to(Gfx::FloatPoint const&) = 0;
    virtual void line_to(Gfx::FloatPoint const&) = 0;
    virtual void close_all_subpaths() = 0;
    virtual void close() = 0;
    virtual void elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep) = 0;
    virtual void arc_to(FloatPoint point, float radius, bool large_arc, bool sweep) = 0;
    virtual void quadratic_bezier_curve_to(FloatPoint through, FloatPoint point) = 0;
    virtual void cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2) = 0;
    virtual void text(Utf8View, Font const&) = 0;

    virtual void append_path(Gfx::Path const&) = 0;
    virtual void intersect(Gfx::Path const&) = 0;

    [[nodiscard]] virtual bool is_empty() const = 0;
    virtual Gfx::FloatPoint last_point() const = 0;
    virtual Gfx::FloatRect bounding_box() const = 0;
    virtual void set_fill_type(Gfx::WindingRule winding_rule) = 0;
    virtual bool contains(FloatPoint point, Gfx::WindingRule) const = 0;

    virtual NonnullOwnPtr<PathImpl> clone() const = 0;
    virtual NonnullOwnPtr<PathImpl> copy_transformed(Gfx::AffineTransform const&) const = 0;
    virtual NonnullOwnPtr<PathImpl> place_text_along(Utf8View text, Font const&) const = 0;
};

class Path {
public:
    Path() = default;

    Path(Path const& other)
        : m_impl(other.impl().clone())
    {
    }

    Path& operator=(Path const& other)
    {
        if (this != &other)
            m_impl = other.impl().clone();
        return *this;
    }

    Path(Path&& other) = default;
    Path& operator=(Path&& other) = default;

    void clear() { impl().clear(); }
    void move_to(Gfx::FloatPoint const& point) { impl().move_to(point); }
    void line_to(Gfx::FloatPoint const& point) { impl().line_to(point); }
    void close_all_subpaths() { impl().close_all_subpaths(); }
    void close() { impl().close(); }
    void elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep) { impl().elliptical_arc_to(point, radii, x_axis_rotation, large_arc, sweep); }
    void arc_to(FloatPoint point, float radius, bool large_arc, bool sweep) { impl().arc_to(point, radius, large_arc, sweep); }
    void quadratic_bezier_curve_to(FloatPoint through, FloatPoint point) { impl().quadratic_bezier_curve_to(through, point); }
    void cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2) { impl().cubic_bezier_curve_to(c1, c2, p2); }
    void text(Utf8View text, Font const& font) { impl().text(text, font); }

    void horizontal_line_to(float x) { line_to({ x, last_point().y() }); }
    void vertical_line_to(float y) { line_to({ last_point().x(), y }); }

    void append_path(Gfx::Path const& other) { impl().append_path(other); }
    void intersect(Gfx::Path const& other) { impl().intersect(other); }

    [[nodiscard]] bool is_empty() const { return impl().is_empty(); }
    Gfx::FloatPoint last_point() const { return impl().last_point(); }
    Gfx::FloatRect bounding_box() const { return impl().bounding_box(); }
    bool contains(FloatPoint point, Gfx::WindingRule winding_rule) const { return impl().contains(point, winding_rule); }
    void set_fill_type(Gfx::WindingRule winding_rule) { impl().set_fill_type(winding_rule); }

    Gfx::Path clone() const { return Gfx::Path { impl().clone() }; }
    Gfx::Path copy_transformed(Gfx::AffineTransform const& transform) const { return Gfx::Path { impl().copy_transformed(transform) }; }
    Gfx::Path place_text_along(Utf8View text, Font const& font) const { return Gfx::Path { impl().place_text_along(text, font) }; }

    void transform(Gfx::AffineTransform const& transform) { m_impl = impl().copy_transformed(transform); }

    PathImpl& impl() { return *m_impl; }
    PathImpl const& impl() const { return *m_impl; }

private:
    explicit Path(NonnullOwnPtr<PathImpl>&& impl)
        : m_impl(move(impl))
    {
    }

    NonnullOwnPtr<PathImpl> m_impl { PathImpl::create() };
};

}
