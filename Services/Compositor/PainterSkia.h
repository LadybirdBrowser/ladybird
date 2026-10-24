/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/WindingRule.h>

class SkColorSpace;
class SkFont;
class SkImage;
class SkImageFilter;
template<typename T>
class sk_sp;

namespace Gfx {

struct CanvasGlyph {
    FloatPoint position;
    u32 glyph_id { 0 };
};

class PainterSkia final {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit PainterSkia(NonnullRefPtr<Gfx::PaintingSurface>);
    ~PainterSkia();

    void clear_rect(Gfx::FloatRect const&, Color);
    void fill_rect(Gfx::FloatRect const&, Color);
    void draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::DecodedImageFrame const& source, Gfx::IntRect const& src_rect, Gfx::ScalingMode, sk_sp<SkImageFilter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);
    void draw_image(SkImage const&, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode, sk_sp<SkImageFilter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);
    void stroke_path(Gfx::Path const&, Gfx::Color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle, Gfx::Path::JoinStyle, float miter_limit, Vector<float> const& dash_array, float dash_offset);
    void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, sk_sp<SkImageFilter>, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const&, Gfx::Path::JoinStyle const&, float miter_limit, Vector<float> const&, float dash_offset);
    void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);
    void fill_path(Gfx::Path const&, Gfx::PaintStyle const&, sk_sp<SkImageFilter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule);
    void draw_glyph_run(Font const&, SkFont, ReadonlySpan<CanvasGlyph>, FloatPoint translation, PaintStyle const&, sk_sp<SkImageFilter>, float global_alpha, CompositingAndBlendingOperator);
    void set_transform(Gfx::AffineTransform const&);
    void save();
    void restore();
    void clip(Gfx::Path const&, Gfx::WindingRule);
    void reset();

private:
    // NB: The colors that the painter is given are in the color space of the surface.
    SkColorSpace* color_space() const;

    NonnullRefPtr<PaintingSurface> m_painting_surface;
    u32 m_initial_save_count { 0 };
};

}
