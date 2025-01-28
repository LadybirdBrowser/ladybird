/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Painter.h>
#include <LibGfx/PaintingSurface.h>

namespace Gfx {

class PainterSkia final : public Painter {
public:
    explicit PainterSkia(NonnullRefPtr<Gfx::PaintingSurface>);
    virtual ~PainterSkia() override;

    virtual void clear_rect(Gfx::FloatRect const&, Color) override;
    virtual void fill_rect(Gfx::FloatRect const&, Color) override;
    virtual void draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::ImmutableBitmap const& src_bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode, ReadonlySpan<Gfx::Filter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::Color, float thickness) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::Color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, ReadonlySpan<Gfx::Filter>, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule) override;
    virtual void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void fill_path(Gfx::Path const&, Gfx::PaintStyle const&, ReadonlySpan<Gfx::Filter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule) override;
    virtual void set_transform(Gfx::AffineTransform const&) override;
    virtual void save() override;
    virtual void restore() override;
    virtual void clip(Gfx::Path const&, Gfx::WindingRule) override;

private:
    struct Impl;
    Impl& impl() { return *m_impl; }
    NonnullOwnPtr<Impl> m_impl;
};

}
