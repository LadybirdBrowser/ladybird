/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Forward.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

class Painter {
public:
    static NonnullOwnPtr<Gfx::Painter> create(NonnullRefPtr<Gfx::Bitmap>);

    virtual ~Painter();

    virtual void clear_rect(Gfx::FloatRect const&, Gfx::Color) = 0;
    virtual void fill_rect(Gfx::FloatRect const&, Gfx::Color) = 0;

    virtual void draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::ImmutableBitmap const& src_bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode, ReadonlySpan<Gfx::Filter> filters, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) = 0;

    virtual void stroke_path(Gfx::Path const&, Gfx::Color, float thickness) = 0;
    virtual void stroke_path(Gfx::Path const&, Gfx::Color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) = 0;
    virtual void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, ReadonlySpan<Gfx::Filter>, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) = 0;
    virtual void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, ReadonlySpan<Gfx::Filter>, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const&, Gfx::Path::JoinStyle const&) = 0;

    virtual void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule) = 0;
    virtual void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) = 0;
    virtual void fill_path(Gfx::Path const&, Gfx::PaintStyle const&, ReadonlySpan<Gfx::Filter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule) = 0;

    virtual void set_transform(Gfx::AffineTransform const&) = 0;

    virtual void save() = 0;
    virtual void restore() = 0;

    virtual void clip(Gfx::Path const&, Gfx::WindingRule) = 0;
};

}
