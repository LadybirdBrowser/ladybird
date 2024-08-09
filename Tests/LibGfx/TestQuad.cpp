/*
 * Copyright (c) 2024, Aaron Van Doren <aaronvandoren6@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Point.h>
#include <LibGfx/Quad.h>
#include <LibGfx/Rect.h>
#include <LibTest/TestCase.h>

TEST_CASE(quad_points)
{
    uint8_t quad_x_left = 1;
    uint8_t quad_x_right = 5;
    uint8_t quad_y_top = 10;
    uint8_t quad_y_bottom = 6;

    Gfx::Point<uint8_t> left_bottom { quad_x_left, quad_y_bottom };
    Gfx::Point<uint8_t> left_top { quad_x_left, quad_y_top };
    Gfx::Point<uint8_t> right_bottom { quad_x_right, quad_y_bottom };
    Gfx::Point<uint8_t> right_top { quad_x_right, quad_y_top };

    Gfx::Quad<uint8_t> quad { left_bottom, left_top, right_bottom, right_top };
    EXPECT_EQ(quad.p1(), left_bottom);
    EXPECT_EQ(quad.p2(), left_top);
    EXPECT_EQ(quad.p3(), right_bottom);
    EXPECT_EQ(quad.p4(), right_top);
}

TEST_CASE(quad_bounding_rect)
{
    uint8_t quad_width = 5;
    uint8_t quad_height = 4;
    uint8_t quad_x_left = 0;
    uint8_t quad_y_top = 6;

    uint8_t quad_x_right = quad_x_left + quad_width;
    uint8_t quad_y_bottom = quad_y_top + quad_height;

    Gfx::Point<uint8_t> left_bottom { quad_x_left, quad_y_bottom };
    Gfx::Point<uint8_t> left_top { quad_x_left, quad_y_top };
    Gfx::Point<uint8_t> right_bottom { quad_x_right, quad_y_bottom };
    Gfx::Point<uint8_t> right_top { quad_x_right, quad_y_top };
    Gfx::Quad<uint8_t> quad = { left_bottom, left_top, right_top, right_bottom };

    auto bounding_rect = quad.bounding_rect();
    EXPECT_EQ(bounding_rect.x(), quad_x_left);
    EXPECT_EQ(bounding_rect.y(), quad_y_top);
    EXPECT_EQ(bounding_rect.width(), quad_width);
    EXPECT_EQ(bounding_rect.height(), quad_height);
}

TEST_CASE(quad_contains)
{
    u8 quad_width = 5;
    u8 quad_height = 4;
    u8 quad_x_left = 0;
    u8 quad_y_top = 6;

    u8 quad_x_right = quad_x_left + quad_width;
    u8 quad_y_bottom = quad_y_top + quad_height;

    Gfx::Point<u8> left_bottom { quad_x_left, quad_y_bottom };
    Gfx::Point<u8> left_top { quad_x_left, quad_y_top };
    Gfx::Point<u8> right_bottom { quad_x_right, quad_y_bottom };
    Gfx::Point<u8> right_top { quad_x_right, quad_y_top };
    Gfx::Quad<u8> quad = { left_bottom, left_top, right_top, right_bottom };

    Gfx::Point<u8> in_bounds_point { 1, 7 };
    EXPECT(quad.contains(in_bounds_point) == true);

    Gfx::Point<u8> out_bounds_point { 7, 12 };
    EXPECT(quad.contains(out_bounds_point) == false);
}

TEST_CASE(quad_contains_boundary_points)
{
    Gfx::Point<int> p1 { 0, 0 };
    Gfx::Point<int> p2 { 2, 0 };
    Gfx::Point<int> p3 { 2, 2 };
    Gfx::Point<int> p4 { 0, 2 };
    Gfx::Quad<int> square_quad { p1, p2, p3, p4 };
    Gfx::Rect<int> square_quad_as_rect = square_quad.bounding_rect();

    auto const& point_at_top_left_vertex = p1;
    Gfx::Point<int> point_on_top_edge { 1, 0 };
    EXPECT_EQ(square_quad.contains(point_at_top_left_vertex), square_quad_as_rect.contains(point_at_top_left_vertex));
    EXPECT_EQ(square_quad.contains(point_on_top_edge), square_quad_as_rect.contains(point_on_top_edge));

    auto const& point_at_top_right_vertex = p2;
    Gfx::Point<int> point_on_right_edge { 2, 1 };
    EXPECT_EQ(square_quad.contains(point_at_top_right_vertex), square_quad_as_rect.contains(point_at_top_right_vertex));
    EXPECT_EQ(square_quad.contains(point_on_right_edge), square_quad_as_rect.contains(point_on_right_edge));

    auto const& point_at_bottom_left_vertex = p4;
    Gfx::Point<int> point_on_bottom_edge { 1, 2 };
    EXPECT_EQ(square_quad.contains(point_at_bottom_left_vertex), square_quad_as_rect.contains(point_at_bottom_left_vertex));
    EXPECT_EQ(square_quad.contains(point_on_bottom_edge), square_quad_as_rect.contains(point_on_bottom_edge));

    auto const& point_at_bottom_right_vertex = p3;
    Gfx::Point<int> point_on_left_edge { 0, 1 };
    EXPECT_EQ(square_quad.contains(point_at_bottom_right_vertex), square_quad_as_rect.contains(point_at_bottom_right_vertex));
    EXPECT_EQ(square_quad.contains(point_on_left_edge), square_quad_as_rect.contains(point_on_left_edge));
}
