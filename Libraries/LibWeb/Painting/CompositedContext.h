/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <LibGfx/Matrix.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Compositor/Types.h>

namespace Web::Painting {

struct CompositedContextSurface {
    RefPtr<Gfx::PaintingSurface> surface;
    // The viewport's pixels, excluding padding needed for fractional raster placement.
    Gfx::FloatRect content_rect;
};

using CompositedContextResolver = Function<CompositedContextSurface(Compositor::CompositorContextId, Gfx::IntRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform)>;

}
