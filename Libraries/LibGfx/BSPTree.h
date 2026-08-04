/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Vector3.h>

namespace Gfx {

// A convex polygon in the shared post-projection space of a three-dimensional scene, where x and y
// are surface coordinates and the positive z-axis points toward the viewer. The plane index
// identifies the plane the polygon was built from and is preserved on pieces produced by splitting.
struct BSPPolygon {
    Vector<FloatVector3, 8> vertices;
    size_t plane_index { 0 };
    bool clipped { false };
};

Vector<FloatVector3, 8> map_rect_through_projection(FloatMatrix4x4 const&, FloatRect const&);

Vector<BSPPolygon> split_and_sort_polygons_back_to_front(Vector<BSPPolygon>);

}
