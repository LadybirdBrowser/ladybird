/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibGfx/BSPTree.h>
#include <LibTest/TestCase.h>

static Gfx::BSPPolygon make_z_plane_polygon(float z, size_t plane_index)
{
    return { { { -10, -10, z }, { 10, -10, z }, { 10, 10, z }, { -10, 10, z } }, plane_index, false };
}

static Gfx::FloatVector3 centroid_of(Gfx::BSPPolygon const& polygon)
{
    Gfx::FloatVector3 sum { 0, 0, 0 };
    for (auto const& vertex : polygon.vertices)
        sum += vertex;
    return sum / static_cast<float>(polygon.vertices.size());
}

TEST_CASE(map_rect_through_projection_maps_corners_in_order)
{
    auto vertices = Gfx::map_rect_through_projection(Gfx::FloatMatrix4x4::identity(), { 10, 20, 30, 40 });
    EXPECT_EQ(vertices.size(), 4u);
    EXPECT_EQ(vertices[0], Gfx::FloatVector3(10, 20, 0));
    EXPECT_EQ(vertices[1], Gfx::FloatVector3(40, 20, 0));
    EXPECT_EQ(vertices[2], Gfx::FloatVector3(40, 60, 0));
    EXPECT_EQ(vertices[3], Gfx::FloatVector3(10, 60, 0));
}

TEST_CASE(map_rect_through_projection_performs_the_perspective_divide)
{
    auto matrix = Gfx::FloatMatrix4x4::identity();
    matrix[3, 0] = 0.015625f;
    auto vertices = Gfx::map_rect_through_projection(matrix, { 0, 0, 64, 32 });
    EXPECT_EQ(vertices.size(), 4u);
    EXPECT_EQ(vertices[0], Gfx::FloatVector3(0, 0, 0));
    EXPECT_EQ(vertices[1], Gfx::FloatVector3(32, 0, 0));
    EXPECT_EQ(vertices[2], Gfx::FloatVector3(32, 16, 0));
    EXPECT_EQ(vertices[3], Gfx::FloatVector3(0, 32, 0));
}

TEST_CASE(map_rect_through_projection_clips_the_region_behind_the_eye)
{
    auto matrix = Gfx::FloatMatrix4x4::identity();
    matrix[3, 0] = -0.03125f;
    auto vertices = Gfx::map_rect_through_projection(matrix, { 0, 0, 64, 32 });
    EXPECT_EQ(vertices.size(), 4u);
    EXPECT_EQ(vertices[0], Gfx::FloatVector3(0, 0, 0));
    EXPECT(vertices[1].x() > 100'000);
    EXPECT(vertices[2].x() > 100'000);
    EXPECT(vertices[2].y() > 100'000);
    EXPECT_EQ(vertices[3], Gfx::FloatVector3(0, 32, 0));
}

TEST_CASE(map_rect_through_projection_drops_a_rect_entirely_behind_the_eye)
{
    auto matrix = Gfx::FloatMatrix4x4::identity();
    matrix[3, 3] = -1;
    EXPECT(Gfx::map_rect_through_projection(matrix, { 0, 0, 64, 32 }).is_empty());
}

TEST_CASE(parallel_planes_sort_back_to_front_for_any_paint_order)
{
    Vector<Gfx::BSPPolygon> polygons;
    for (size_t i = 0; i < 20; ++i)
        polygons.append(make_z_plane_polygon(static_cast<float>((i * 7) % 20), i));

    // Painting proceeds in ascending z, where the largest z is nearest the viewer and paints last.
    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 20u);
    for (size_t i = 0; i < sorted.size(); ++i) {
        EXPECT(!sorted[i].clipped);
        EXPECT_EQ(sorted[i].vertices.first().z(), static_cast<float>(i));
    }
}

TEST_CASE(reversed_winding_does_not_affect_depth_order)
{
    Vector<Gfx::BSPPolygon> polygons;
    polygons.append({ { { -10, 10, 5 }, { 10, 10, 5 }, { 10, -10, 5 }, { -10, -10, 5 } }, 0, false });
    polygons.append(make_z_plane_polygon(-5, 1));

    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 2u);
    EXPECT_EQ(sorted[0].plane_index, 1u);
    EXPECT_EQ(sorted[1].plane_index, 0u);
}

TEST_CASE(coplanar_polygons_keep_paint_order)
{
    Vector<Gfx::BSPPolygon> polygons;
    for (size_t i = 0; i < 3; ++i)
        polygons.append(make_z_plane_polygon(0, i));

    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 3u);
    for (size_t i = 0; i < sorted.size(); ++i)
        EXPECT_EQ(sorted[i].plane_index, i);
}

TEST_CASE(coplanar_polygons_keep_paint_order_when_sorted_against_other_planes)
{
    // The third plane is tilted so the planes do not count as parallel and ordering runs through the tree.
    Vector<Gfx::BSPPolygon> polygons;
    polygons.append(make_z_plane_polygon(0, 0));
    polygons.append(make_z_plane_polygon(0, 1));
    polygons.append({ { { -10, -10, -50.1f }, { 10, -10, -49.9f }, { 10, 10, -49.9f }, { -10, 10, -50.1f } }, 2, false });

    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].plane_index, 2u);
    EXPECT_EQ(sorted[1].plane_index, 0u);
    EXPECT_EQ(sorted[2].plane_index, 1u);
}

TEST_CASE(intersecting_planes_are_split_into_ordered_pieces)
{
    Vector<Gfx::BSPPolygon> polygons;
    polygons.append(make_z_plane_polygon(0, 0));
    // A polygon on the plane z = x, crossing the first polygon along the line x = 0.
    polygons.append({ { { -10, -10, -10 }, { 10, -10, 10 }, { 10, 10, 10 }, { -10, 10, -10 } }, 1, false });

    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 3u);

    // The crossing polygon stays whole and the flat one is cut into a piece on either side of it. The
    // piece with positive x lies behind the crossing plane and paints first.
    EXPECT_EQ(sorted[0].plane_index, 0u);
    EXPECT(sorted[0].clipped);
    EXPECT(centroid_of(sorted[0]).x() > 0);

    EXPECT_EQ(sorted[1].plane_index, 1u);
    EXPECT(!sorted[1].clipped);

    EXPECT_EQ(sorted[2].plane_index, 0u);
    EXPECT(sorted[2].clipped);
    EXPECT(centroid_of(sorted[2]).x() < 0);
}

TEST_CASE(polygons_without_a_plane_are_omitted)
{
    Vector<Gfx::BSPPolygon> polygons;
    polygons.append({ { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }, 0, false });
    polygons.append(make_z_plane_polygon(0, 1));

    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 1u);
    EXPECT_EQ(sorted[0].plane_index, 1u);
}

TEST_CASE(small_distant_parallel_planes_sort_without_splitting)
{
    // Two 8x8 parallel quads far from the origin, two units apart along their shared normal.
    Vector<Gfx::BSPPolygon> polygons;
    polygons.append({ { { 101230.914f, 99866.41f, 3141.205f }, { 101236.88f, 99868.2f, 3146.2156f }, { 101235.51f, 99876.0f, 3145.0588f }, { 101229.54f, 99874.195f, 3140.0483f } }, 0, false });
    polygons.append({ { { 101232.2f, 99866.41f, 3139.673f }, { 101238.17f, 99868.2f, 3144.6836f }, { 101236.8f, 99876.0f, 3143.5269f }, { 101230.83f, 99874.195f, 3138.5164f } }, 1, false });

    auto sorted = Gfx::split_and_sort_polygons_back_to_front(move(polygons));
    EXPECT_EQ(sorted.size(), 2u);
    EXPECT_EQ(sorted[0].plane_index, 1u);
    EXPECT(!sorted[0].clipped);
    EXPECT_EQ(sorted[1].plane_index, 0u);
    EXPECT(!sorted[1].clipped);
}
