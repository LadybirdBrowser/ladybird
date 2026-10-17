/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
