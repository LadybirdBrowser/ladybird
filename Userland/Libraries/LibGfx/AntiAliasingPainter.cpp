/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, Ben Maxwell <macdue@dueutil.tech>
 * Copyright (c) 2022, Torsten Engelmann <engelTorsten@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

#include <AK/Function.h>
#include <AK/NumericLimits.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/Line.h>

namespace Gfx {

void AntiAliasingPainter::stroke_path(DeprecatedPath const& path, Color color, float thickness)
{
    if (thickness <= 0)
        return;
    // FIXME: Cache this? Probably at a higher level such as in LibWeb?
    fill_path(path.stroke_to_fill(thickness), color);
}

void AntiAliasingPainter::stroke_path(DeprecatedPath const& path, Gfx::PaintStyle const& paint_style, float thickness, float opacity)
{
    if (thickness <= 0)
        return;
    // FIXME: Cache this? Probably at a higher level such as in LibWeb?
    fill_path(path.stroke_to_fill(thickness), paint_style, opacity);
}

}
