/*
 * Copyright (c) 2025, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Line.h>
#include <LibTest/TestCase.h>

TEST_CASE(line_endpoint_intersection)
{
    Gfx::Line<float> a({ 0.f, 0.f }, { 10.f, 0.f });
    Gfx::Line<float> b({ 10.f, 0.f }, { 10.f, 10.f });

    EXPECT(a.intersects(b));

    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 10.f);
    EXPECT_APPROXIMATE(point->y(), 0.f);
}

TEST_CASE(line_no_intersection_parallel)
{
    Gfx::Line<float> a({ 0.f, 0.f }, { 10.f, 0.f });
    Gfx::Line<float> b({ 0.f, 1.f }, { 10.f, 1.f });

    EXPECT(!a.intersects(b));
    EXPECT(!a.intersected(b).has_value());
}

TEST_CASE(line_proper_intersection)
{
    Gfx::Line<float> a({ 0.f, 0.f }, { 10.f, 10.f });
    Gfx::Line<float> b({ 0.f, 10.f }, { 10.f, 0.f });

    EXPECT(a.intersects(b));

    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 5.f);
    EXPECT_APPROXIMATE(point->y(), 5.f);
}

TEST_CASE(line_overlap_total_containment_horizontal)
{
    // A is totally contained within B (horizontal collinear)
    Gfx::Line<float> a({ 2.f, 0.f }, { 8.f, 0.f });
    Gfx::Line<float> b({ 0.f, 0.f }, { 10.f, 0.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 5.f);
    EXPECT_APPROXIMATE(point->y(), 0.f);
}

TEST_CASE(line_overlap_total_containment_diagonal)
{
    // A is totally contained within B (diagonal collinear)
    Gfx::Line<float> a({ 2.f, 2.f }, { 8.f, 8.f });
    Gfx::Line<float> b({ 0.f, 0.f }, { 10.f, 10.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 5.f);
    EXPECT_APPROXIMATE(point->y(), 5.f);
}

TEST_CASE(line_collinear_no_overlap)
{
    // 2 horizontal collinear segments that do not overlap
    Gfx::Line<float> a({ 0.f, 0.f }, { 1.f, 0.f });
    Gfx::Line<float> b({ 2.f, 0.f }, { 3.f, 0.f });

    EXPECT(!a.intersects(b));
    EXPECT(!a.intersected(b).has_value());
}

TEST_CASE(line_collinear_perfect_overlap)
{
    // Identical segments (reversed)
    Gfx::Line<float> a({ -37.25f, 12.0f }, { 18.5f, -9.75f });
    Gfx::Line<float> b({ 18.5f, -9.75f }, { -37.25f, 12.0f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());

    // Should intersect at their midpoint
    float mx = (a.a().x() + a.b().x()) / 2.f;
    float my = (a.a().y() + a.b().y()) / 2.f;
    EXPECT_APPROXIMATE(point->x(), mx);
    EXPECT_APPROXIMATE(point->y(), my);
}

TEST_CASE(line_overlap_partial_left_extrude)
{
    // A overlaps B and extends beyond B's start (left extrude)
    Gfx::Line<float> a({ -5.f, 0.f }, { 5.f, 0.f });
    Gfx::Line<float> b({ 0.f, 0.f }, { 10.f, 0.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 2.5f);
    EXPECT_APPROXIMATE(point->y(), 0.f);
}

TEST_CASE(line_overlap_partial_right_extrude)
{
    // A overlaps B and extends beyond B's end (right extrude)
    Gfx::Line<float> a({ 5.f, 0.f }, { 15.f, 0.f });
    Gfx::Line<float> b({ 0.f, 0.f }, { 10.f, 0.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 7.5f);
    EXPECT_APPROXIMATE(point->y(), 0.f);
}

TEST_CASE(line_collinear_shared_endpoint_horizontal)
{
    // Collinear segments touching at one endpoint only
    Gfx::Line<float> a({ 0.f, 0.f }, { 5.f, 0.f });
    Gfx::Line<float> b({ 5.f, 0.f }, { 10.f, 0.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 5.f);
    EXPECT_APPROXIMATE(point->y(), 0.f);
}

TEST_CASE(line_collinear_shared_endpoint_diagonal)
{
    // Diagonal collinear segments touching at one endpoint only
    Gfx::Line<float> a({ 0.f, 0.f }, { 5.f, 5.f });
    Gfx::Line<float> b({ 5.f, 5.f }, { 10.f, 10.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 5.f);
    EXPECT_APPROXIMATE(point->y(), 5.f);
}

TEST_CASE(line_point_on_segment)
{
    // Zero-length segment A lies on B
    Gfx::Line<float> a({ 5.f, 0.f }, { 5.f, 0.f });
    Gfx::Line<float> b({ 0.f, 0.f }, { 10.f, 0.f });

    EXPECT(a.intersects(b));
    auto point = a.intersected(b);
    EXPECT(point.has_value());
    EXPECT_APPROXIMATE(point->x(), 5.f);
    EXPECT_APPROXIMATE(point->y(), 0.f);
}

TEST_CASE(line_point_off_segment)
{
    // Zero-length segment A not on B
    Gfx::Line<float> a({ 11.f, 0.f }, { 11.f, 0.f });
    Gfx::Line<float> b({ 0.f, 0.f }, { 10.f, 0.f });

    EXPECT(!a.intersects(b));
    EXPECT(!a.intersected(b).has_value());
}
