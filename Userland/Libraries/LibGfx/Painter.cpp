/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Painter.h>
#include <LibGfx/PainterSkia.h>

namespace Gfx {

Painter::~Painter() = default;

NonnullOwnPtr<Painter> Painter::create(NonnullRefPtr<Gfx::Bitmap> target_bitmap)
{
    return make<PainterSkia>(move(target_bitmap));
}

}
