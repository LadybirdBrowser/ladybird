/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ScalingMode.h>

namespace Gfx {

class Painter {
public:
    static NonnullOwnPtr<Gfx::Painter> create(NonnullRefPtr<Gfx::Bitmap>);

    virtual ~Painter();

    virtual void clear_rect(Gfx::FloatRect const&, Gfx::Color) = 0;
    virtual void fill_rect(Gfx::FloatRect const&, Gfx::Color) = 0;

    virtual void draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::DecodedImageFrame const& source, Gfx::IntRect const& src_rect, Gfx::ScalingMode, Optional<Gfx::Filter> filters, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) = 0;
};

}
