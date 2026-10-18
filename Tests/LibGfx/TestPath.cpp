/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/Path.h>
#include <LibTest/TestCase.h>

static void expect_point_along_path(Gfx::Path const& path, float distance, Gfx::FloatPoint point, Gfx::FloatVector2 tangent)
{
    auto point_along_path = path.point_at_length(distance);
    VERIFY(point_along_path.has_value());
    EXPECT_APPROXIMATE(point_along_path->point.x(), point.x());
    EXPECT_APPROXIMATE(point_along_path->point.y(), point.y());
    EXPECT_APPROXIMATE(point_along_path->tangent.x(), tangent.x());
    EXPECT_APPROXIMATE(point_along_path->tangent.y(), tangent.y());
}

TEST_CASE(point_at_length_along_a_line)
{
    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.line_to({ 100, 0 });

    expect_point_along_path(path, 25, { 25, 0 }, { 1, 0 });
    expect_point_along_path(path, -10, { 0, 0 }, { 1, 0 });
    expect_point_along_path(path, 1000, { 100, 0 }, { 1, 0 });
}

TEST_CASE(point_at_length_skips_the_gaps_between_subpaths)
{
    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.line_to({ 10, 0 });
    path.move_to({ 50, 50 });
    path.move_to({ 100, 100 });
    path.line_to({ 100, 110 });

    expect_point_along_path(path, 5, { 5, 0 }, { 1, 0 });
    expect_point_along_path(path, 15, { 100, 105 }, { 0, 1 });
    expect_point_along_path(path, 25, { 100, 110 }, { 0, 1 });
}

TEST_CASE(point_at_length_along_a_closed_path)
{
    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.line_to({ 10, 0 });
    path.line_to({ 10, 10 });
    path.line_to({ 0, 10 });
    path.close();

    expect_point_along_path(path, 15, { 10, 5 }, { 0, 1 });
    expect_point_along_path(path, 35, { 0, 5 }, { 0, -1 });
}

TEST_CASE(point_at_length_of_a_path_without_length)
{
    Gfx::Path path;
    path.move_to({ 10, 20 });
    expect_point_along_path(path, 5, { 10, 20 }, { 0, 0 });

    EXPECT(!Gfx::Path {}.point_at_length(0).has_value());
}

static void expect_vertices(Gfx::Path const& path, Vector<Gfx::PathVertex> const& expected_vertices)
{
    auto vertices = path.vertices();
    VERIFY(vertices.size() == expected_vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        EXPECT_APPROXIMATE_WITH_ERROR(vertices[i].point.x(), expected_vertices[i].point.x(), 0.0001f);
        EXPECT_APPROXIMATE_WITH_ERROR(vertices[i].point.y(), expected_vertices[i].point.y(), 0.0001f);
        EXPECT_APPROXIMATE_WITH_ERROR(vertices[i].distance, expected_vertices[i].distance, 0.0001f);
        EXPECT_APPROXIMATE_WITH_ERROR(vertices[i].direction.x(), expected_vertices[i].direction.x(), 0.0001f);
        EXPECT_APPROXIMATE_WITH_ERROR(vertices[i].direction.y(), expected_vertices[i].direction.y(), 0.0001f);
    }
}

TEST_CASE(vertices_of_subpaths_of_lines)
{
    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.line_to({ 10, 0 });
    path.line_to({ 10, 10 });
    path.close();
    path.move_to({ 50, 50 });
    path.line_to({ 60, 50 });

    auto diagonal = AK::sqrt(200.f);
    auto inverse_sqrt_2 = 1 / AK::sqrt(2.f);
    expect_vertices(path,
        {
            { { 0, 0 }, 0, { 1, 0 } },
            { { 10, 0 }, 10, { 1, 0 } },
            { { 10, 0 }, 10, { 0, 1 } },
            { { 10, 10 }, 20, { 0, 1 } },
            { { 10, 10 }, 20, { -inverse_sqrt_2, -inverse_sqrt_2 } },
            { { 0, 0 }, 20 + diagonal, { -inverse_sqrt_2, -inverse_sqrt_2 } },
            { { 50, 50 }, 20 + diagonal, { 1, 0 } },
            { { 60, 50 }, 30 + diagonal, { 1, 0 } },
        });
}

TEST_CASE(vertices_along_curves)
{
    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.quadratic_bezier_curve_to({ 50, 0 }, { 100, 0 });
    expect_vertices(path, { { { 0, 0 }, 0, { 1, 0 } }, { { 100, 0 }, 100, { 1, 0 } } });

    Gfx::Path long_path;
    long_path.move_to({ 0, 0 });
    long_path.quadratic_bezier_curve_to({ 10'000'000, 0 }, { 20'000'000, 0 });
    expect_vertices(long_path, { { { 0, 0 }, 0, { 1, 0 } }, { { 20'000'000, 0 }, 20'000'000, { 1, 0 } } });

    Gfx::Path circle;
    circle.move_to({ 10, 0 });
    circle.arc_to({ -10, 0 }, 10, false, true);
    circle.arc_to({ 10, 0 }, 10, false, true);
    auto vertices = circle.vertices();
    EXPECT(vertices.size() > 40);
    for (auto const& vertex : vertices) {
        EXPECT_APPROXIMATE_WITH_ERROR(vertex.point.distance_from({ 0, 0 }), 10.f, 0.001f);
        EXPECT_APPROXIMATE_WITH_ERROR(vertex.direction.length(), 1.f, 0.001f);
        EXPECT_APPROXIMATE_WITH_ERROR(vertex.direction.dot({ vertex.point.x(), vertex.point.y() }), 0.f, 0.01f);
    }
    for (size_t i = 1; i < vertices.size(); ++i) {
        auto midpoint = (vertices[i - 1].point + vertices[i].point) / 2;
        EXPECT(midpoint.distance_from({ 0, 0 }) > 10.f - 0.5f / 16);
    }
    EXPECT_APPROXIMATE(vertices.first().point.x(), 10.f);
    EXPECT_APPROXIMATE(vertices.last().point.x(), 10.f);
    EXPECT_APPROXIMATE_WITH_ERROR(vertices.last().distance, 20 * AK::Pi<float>, 0.05f);
}

TEST_CASE(vertices_of_a_path_without_length)
{
    Gfx::Path path;
    path.move_to({ 10, 20 });
    expect_vertices(path, { { { 10, 20 }, 0, {} } });

    Gfx::Path zero_length_line;
    zero_length_line.move_to({ 10, 20 });
    zero_length_line.line_to({ 10, 20 });
    expect_vertices(zero_length_line, { { { 10, 20 }, 0, {} } });

    expect_vertices(Gfx::Path {}, {});
}
