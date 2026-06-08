/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibGfx/Path.h>
#include <LibWeb/SVG/Path.h>

namespace Web::SVG {

static void serialize_path_instruction(PathInstruction const& instruction, StringBuilder& builder)
{
    instruction.visit(
        [&](MoveToInstruction const& move_to) {
            builder.appendff(
                "{} {} {}",
                move_to.absolute ? 'M' : 'm',
                move_to.point.x(),
                move_to.point.y());
        },
        [&](ClosePathInstruction const&) {
            // NB: This is always canonicalized as Z, not z.
            builder.append('Z');
        },
        [&](LineToInstruction const& line_to) {
            builder.appendff(
                "{} {} {}",
                line_to.absolute ? 'L' : 'l',
                line_to.point.x(),
                line_to.point.y());
        },
        [&](HorizontalLineToInstruction const& horizontal_line_to) {
            builder.appendff(
                "{} {}",
                horizontal_line_to.absolute ? 'H' : 'h',
                horizontal_line_to.x);
        },
        [&](VerticalLineToInstruction const& vertical_line_to) {
            builder.appendff(
                "{} {}",
                vertical_line_to.absolute ? 'V' : 'v',
                vertical_line_to.y);
        },
        [&](CurveToInstruction const& curve_to) {
            builder.appendff(
                "{} {} {} {} {} {} {}",
                curve_to.absolute ? 'C' : 'c',
                curve_to.control_point_1.x(),
                curve_to.control_point_1.y(),
                curve_to.control_point_2.x(),
                curve_to.control_point_2.y(),
                curve_to.point.x(),
                curve_to.point.y());
        },
        [&](SmoothCurveToInstruction const& smooth_curve_to) {
            builder.appendff(
                "{} {} {} {} {}",
                smooth_curve_to.absolute ? 'S' : 's',
                smooth_curve_to.control_point_2.x(),
                smooth_curve_to.control_point_2.y(),
                smooth_curve_to.point.x(),
                smooth_curve_to.point.y());
        },
        [&](QuadraticBezierCurveToInstruction const& quadratic_bezier_curve_to) {
            builder.appendff(
                "{} {} {} {} {}",
                quadratic_bezier_curve_to.absolute ? 'Q' : 'q',
                quadratic_bezier_curve_to.control_point.x(),
                quadratic_bezier_curve_to.control_point.y(),
                quadratic_bezier_curve_to.point.x(),
                quadratic_bezier_curve_to.point.y());
        },
        [&](SmoothQuadraticBezierCurveToInstruction const& smooth_quadratic_bezier_curve_to) {
            builder.appendff(
                "{} {} {}",
                smooth_quadratic_bezier_curve_to.absolute ? 'T' : 't',
                smooth_quadratic_bezier_curve_to.point.x(),
                smooth_quadratic_bezier_curve_to.point.y());
        },
        [&](EllipticalArcInstruction const& elliptical_arc) {
            builder.appendff(
                "{} {} {} {} {} {} {} {}",
                elliptical_arc.absolute ? 'A' : 'a',
                elliptical_arc.rx,
                elliptical_arc.ry,
                elliptical_arc.x_axis_rotation,
                elliptical_arc.large_arc ? 1 : 0,
                elliptical_arc.sweep ? 1 : 0,
                elliptical_arc.point.x(),
                elliptical_arc.point.y());
        });
}

[[maybe_unused]] static void dump_path_instruction(PathInstruction const& instruction)
{
    instruction.visit(
        [](MoveToInstruction const& move_to) {
            dbgln("Move (absolute={})", move_to.absolute);
            dbgln("    x={}, y={}", move_to.point.x(), move_to.point.y());
        },
        [](ClosePathInstruction const&) {
            dbgln("ClosePath");
        },
        [](LineToInstruction const& line_to) {
            dbgln("Line (absolute={})", line_to.absolute);
            dbgln("    x={}, y={}", line_to.point.x(), line_to.point.y());
        },
        [](HorizontalLineToInstruction const& horizontal_line_to) {
            dbgln("HorizontalLine (absolute={})", horizontal_line_to.absolute);
            dbgln("    x={}", horizontal_line_to.x);
        },
        [](VerticalLineToInstruction const& vertical_line_to) {
            dbgln("VerticalLine (absolute={})", vertical_line_to.absolute);
            dbgln("    y={}", vertical_line_to.y);
        },
        [](CurveToInstruction const& curve_to) {
            dbgln("Curve (absolute={})", curve_to.absolute);
            dbgln("    (x1={}, y1={}, x2={}, y2={}), (x={}, y={})", curve_to.control_point_1.x(), curve_to.control_point_1.y(), curve_to.control_point_2.x(), curve_to.control_point_2.y(), curve_to.point.x(), curve_to.point.y());
        },
        [](SmoothCurveToInstruction const& smooth_curve_to) {
            dbgln("SmoothCurve (absolute={})", smooth_curve_to.absolute);
            dbgln("    (x2={}, y2={}), (x={}, y={})", smooth_curve_to.control_point_2.x(), smooth_curve_to.control_point_2.y(), smooth_curve_to.point.x(), smooth_curve_to.point.y());
        },
        [](QuadraticBezierCurveToInstruction const& quadratic_bezier_curve_to) {
            dbgln("QuadraticBezierCurve (absolute={})", quadratic_bezier_curve_to.absolute);
            dbgln("    (x1={}, y1={}), (x={}, y={})", quadratic_bezier_curve_to.control_point.x(), quadratic_bezier_curve_to.control_point.y(), quadratic_bezier_curve_to.point.x(), quadratic_bezier_curve_to.point.y());
        },
        [](SmoothQuadraticBezierCurveToInstruction const& smooth_quadratic_bezier_curve_to) {
            dbgln("SmoothQuadraticBezierCurve (absolute={})", smooth_quadratic_bezier_curve_to.absolute);
            dbgln("    x={}, y={}", smooth_quadratic_bezier_curve_to.point.x(), smooth_quadratic_bezier_curve_to.point.y());
        },
        [](EllipticalArcInstruction const& elliptical_arc) {
            dbgln("EllipticalArc (absolute={})", elliptical_arc.absolute);
            dbgln("    (rx={}, ry={}) x-axis-rotation={}, large-arc-flag={}, sweep-flag={}, (x={}, y={})",
                elliptical_arc.rx,
                elliptical_arc.ry,
                elliptical_arc.x_axis_rotation,
                elliptical_arc.large_arc,
                elliptical_arc.sweep,
                elliptical_arc.point.x(),
                elliptical_arc.point.y());
        });
}

Gfx::Path Path::to_gfx_path() const
{
    Gfx::Path path;
    Optional<Gfx::FloatPoint> previous_control_point;
    PathInstruction const* last_instruction = nullptr;

    for (auto& instruction : m_instructions) {
        // If the first path element uses relative coordinates, we treat them as absolute by making them relative to (0, 0).
        auto last_point = path.last_point();

        if constexpr (PATH_DEBUG) {
            dump_path_instruction(instruction);
        }

        bool clear_last_control_point = true;

        instruction.visit(
            [&](MoveToInstruction const& move_to_instruction) {
                if (move_to_instruction.absolute) {
                    path.move_to(move_to_instruction.point);
                } else {
                    path.move_to(move_to_instruction.point + last_point);
                }
            },
            [&](ClosePathInstruction const&) {
                path.close();
            },
            [&](LineToInstruction const& line_to_instruction) {
                if (line_to_instruction.absolute) {
                    path.line_to(line_to_instruction.point);
                } else {
                    path.line_to(line_to_instruction.point + last_point);
                }
            },
            [&](HorizontalLineToInstruction const& horizontal_line_to_instruction) {
                if (horizontal_line_to_instruction.absolute)
                    path.line_to(Gfx::FloatPoint { horizontal_line_to_instruction.x, last_point.y() });
                else
                    path.line_to(Gfx::FloatPoint { horizontal_line_to_instruction.x + last_point.x(), last_point.y() });
            },
            [&](VerticalLineToInstruction const& vertical_line_to_instruction) {
                if (vertical_line_to_instruction.absolute)
                    path.line_to(Gfx::FloatPoint { last_point.x(), vertical_line_to_instruction.y });
                else
                    path.line_to(Gfx::FloatPoint { last_point.x(), vertical_line_to_instruction.y + last_point.y() });
            },
            [&](EllipticalArcInstruction const& elliptical_arc_instruction) {
                auto x_axis_rotation = AK::to_radians(static_cast<double>(elliptical_arc_instruction.x_axis_rotation));

                Gfx::FloatPoint next_point;

                if (elliptical_arc_instruction.absolute)
                    next_point = elliptical_arc_instruction.point;
                else
                    next_point = elliptical_arc_instruction.point + last_point;

                path.elliptical_arc_to(next_point, { elliptical_arc_instruction.rx, elliptical_arc_instruction.ry }, x_axis_rotation, elliptical_arc_instruction.large_arc, elliptical_arc_instruction.sweep);
            },
            [&](QuadraticBezierCurveToInstruction const& quadratic_bezier_curve_to_instruction) {
                clear_last_control_point = false;

                if (quadratic_bezier_curve_to_instruction.absolute) {
                    path.quadratic_bezier_curve_to(quadratic_bezier_curve_to_instruction.control_point, quadratic_bezier_curve_to_instruction.point);
                    previous_control_point = quadratic_bezier_curve_to_instruction.control_point;
                } else {
                    auto control_point = quadratic_bezier_curve_to_instruction.control_point + last_point;
                    path.quadratic_bezier_curve_to(control_point, quadratic_bezier_curve_to_instruction.point + last_point);
                    previous_control_point = control_point;
                }
            },
            [&](SmoothQuadraticBezierCurveToInstruction const& smooth_quadratic_bezier_curve_to_instruction) {
                clear_last_control_point = false;

                if (!previous_control_point.has_value()
                    || (!last_instruction || (!last_instruction->has<QuadraticBezierCurveToInstruction>() && !last_instruction->has<SmoothQuadraticBezierCurveToInstruction>()))) {
                    previous_control_point = last_point;
                }

                auto dx_end_control = last_point.dx_relative_to(previous_control_point.value());
                auto dy_end_control = last_point.dy_relative_to(previous_control_point.value());
                auto control_point = Gfx::FloatPoint { last_point.x() + dx_end_control, last_point.y() + dy_end_control };

                if (smooth_quadratic_bezier_curve_to_instruction.absolute) {
                    path.quadratic_bezier_curve_to(control_point, smooth_quadratic_bezier_curve_to_instruction.point);
                } else {
                    path.quadratic_bezier_curve_to(control_point, smooth_quadratic_bezier_curve_to_instruction.point + last_point);
                }

                previous_control_point = control_point;
            },
            [&](CurveToInstruction const& curve_to_instruction) {
                clear_last_control_point = false;

                Gfx::FloatPoint c1 = curve_to_instruction.control_point_1;
                Gfx::FloatPoint c2 = curve_to_instruction.control_point_2;
                Gfx::FloatPoint p2 = curve_to_instruction.point;

                if (!curve_to_instruction.absolute) {
                    p2 += last_point;
                    c1 += last_point;
                    c2 += last_point;
                }
                path.cubic_bezier_curve_to(c1, c2, p2);
                previous_control_point = c2;
            },
            [&](SmoothCurveToInstruction const& smooth_curve_to_instruction) {
                clear_last_control_point = false;

                if (!previous_control_point.has_value()
                    || (!last_instruction || (!last_instruction->has<CurveToInstruction>() && !last_instruction->has<SmoothCurveToInstruction>()))) {
                    previous_control_point = last_point;
                }

                // 9.5.2. Reflected control points https://svgwg.org/svg2-draft/paths.html#ReflectedControlPoints
                // If the current point is (curx, cury) and the final control point of the previous path segment is (oldx2, oldy2),
                // then the reflected point (i.e., (newx1, newy1), the first control point of the current path segment) is:
                // (newx1, newy1) = (curx - (oldx2 - curx), cury - (oldy2 - cury))
                auto reflected_previous_control_x = last_point.x() - previous_control_point.value().dx_relative_to(last_point);
                auto reflected_previous_control_y = last_point.y() - previous_control_point.value().dy_relative_to(last_point);
                Gfx::FloatPoint c1 = Gfx::FloatPoint { reflected_previous_control_x, reflected_previous_control_y };
                Gfx::FloatPoint c2 = smooth_curve_to_instruction.control_point_2;
                Gfx::FloatPoint p2 = smooth_curve_to_instruction.point;
                if (!smooth_curve_to_instruction.absolute) {
                    p2 += last_point;
                    c2 += last_point;
                }
                path.cubic_bezier_curve_to(c1, c2, p2);

                previous_control_point = c2;
            });

        if (clear_last_control_point) {
            previous_control_point = Gfx::FloatPoint {};
        }
        last_instruction = &instruction;
    }

    return path;
}

String Path::serialize() const
{
    StringBuilder builder;
    bool first = true;
    for (auto const& instruction : m_instructions) {
        if (first) {
            first = false;
        } else {
            builder.append(' ');
        }
        serialize_path_instruction(instruction, builder);
    }
    return builder.to_string_without_validation();
}

}
