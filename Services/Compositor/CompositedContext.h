/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Matrix.h>
#include <LibGfx/Rect.h>

namespace Compositor {

struct CompositedContextSurface {
    RefPtr<Gfx::PaintingSurface> surface;
    // The viewport's pixels, excluding padding needed for fractional raster placement.
    Gfx::FloatRect content_rect;
};

using CompositedContextResolver = Function<CompositedContextSurface(Web::CompositorContextId, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform)>;

}
