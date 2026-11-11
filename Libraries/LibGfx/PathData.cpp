/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/PathData.h>
#include <LibGfx/RustFFI.h>

static_assert(sizeof(Gfx::FloatPoint) == sizeof(Gfx::FFI::FloatPoint));
static_assert(to_underlying(Gfx::WindingRule::Nonzero) == 0 && to_underlying(Gfx::WindingRule::EvenOdd) == 1);
static_assert(to_underlying(Gfx::PathVerb::Move) == to_underlying(Gfx::FFI::PathVerb::Move));
static_assert(to_underlying(Gfx::PathVerb::Line) == to_underlying(Gfx::FFI::PathVerb::Line));
static_assert(to_underlying(Gfx::PathVerb::Quad) == to_underlying(Gfx::FFI::PathVerb::Quad));
static_assert(to_underlying(Gfx::PathVerb::Conic) == to_underlying(Gfx::FFI::PathVerb::Conic));
static_assert(to_underlying(Gfx::PathVerb::Cubic) == to_underlying(Gfx::FFI::PathVerb::Cubic));
static_assert(to_underlying(Gfx::PathVerb::Close) == to_underlying(Gfx::FFI::PathVerb::Close));

namespace Gfx {

PathData PathData::from_ffi(FFI::PathGeometryData const& data)
{
    return {
        .verbs = { reinterpret_cast<PathVerb const*>(data.verbs), data.verb_count },
        .points = { reinterpret_cast<FloatPoint const*>(data.points), data.point_count },
        .conic_weights = { data.conic_weights, data.conic_weight_count },
        .winding_rule = static_cast<WindingRule>(data.fill_rule),
    };
}

}
