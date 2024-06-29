/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Vector.h>
#include <LibAccelGfx/Canvas.h>
#include <LibAccelGfx/Context.h>
#include <LibAccelGfx/Forward.h>
#include <LibAccelGfx/GL.h>
#include <LibAccelGfx/GlyphAtlas.h>
#include <LibAccelGfx/Program.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/TextLayout.h>

namespace AccelGfx {

class Painter {
    AK_MAKE_NONCOPYABLE(Painter);
    AK_MAKE_NONMOVABLE(Painter);

public:
    static NonnullOwnPtr<Painter> create(Context&, NonnullRefPtr<Canvas>);

    Painter(Context&, NonnullRefPtr<Canvas>);
    ~Painter();

    Canvas const& canvas() { return *m_target_canvas; }

    void clear(Gfx::Color);

    void save();
    void restore();

    [[nodiscard]] Gfx::AffineTransform const& transform() const { return state().transform; }
    void set_transform(Gfx::AffineTransform const& transform) { state().transform = transform; }
    void translate(Gfx::FloatPoint translation) { state().transform.translate(translation); }

    Gfx::IntRect const& clip_rect() const { return state().clip_rect; }

    void fill_rect(Gfx::FloatRect, Gfx::Color);
    void fill_rect(Gfx::IntRect, Gfx::Color);

    enum class ScalingMode {
        NearestNeighbor,
        Bilinear,
    };

    enum class BlendingMode {
        AlphaAdd,
        AlphaOverride,
        AlphaPreserve,
    };

    void draw_line(Gfx::IntPoint a, Gfx::IntPoint b, float thickness, Gfx::Color color);
    void draw_line(Gfx::FloatPoint a, Gfx::FloatPoint b, float thickness, Gfx::Color color);

    void draw_scaled_bitmap(Gfx::FloatRect const& dst_rect, Gfx::Bitmap const&, Gfx::FloatRect const& src_rect, ScalingMode = ScalingMode::NearestNeighbor);
    void draw_scaled_bitmap(Gfx::IntRect const& dst_rect, Gfx::Bitmap const&, Gfx::IntRect const& src_rect, ScalingMode = ScalingMode::NearestNeighbor);

    void draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::ImmutableBitmap const&, Gfx::IntRect const& src_rect, ScalingMode = ScalingMode::NearestNeighbor);
    void draw_scaled_immutable_bitmap(Gfx::FloatRect const& dst_rect, Gfx::ImmutableBitmap const&, Gfx::FloatRect const& src_rect, ScalingMode = ScalingMode::NearestNeighbor);

    void draw_glyph_run(Span<Gfx::DrawGlyphOrEmoji const> glyph_run, Gfx::Font const&, Color const& color);

    void set_clip_rect(Gfx::IntRect);
    void clear_clip_rect();

    void flush(Gfx::Bitmap&);

    void fill_rect_with_linear_gradient(Gfx::IntRect const&, ReadonlySpan<Gfx::ColorStop>, float angle, Optional<float> repeat_length = {});
    void fill_rect_with_linear_gradient(Gfx::FloatRect const&, ReadonlySpan<Gfx::ColorStop>, float angle, Optional<float> repeat_length = {});

    struct CornerRadius {
        float horizontal_radius;
        float vertical_radius;
    };
    void fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color const& color, CornerRadius const& top_left_radius, CornerRadius const& top_right_radius, CornerRadius const& bottom_left_radius, CornerRadius const& bottom_right_radius, BlendingMode = BlendingMode::AlphaAdd);
    void fill_rect_with_rounded_corners(Gfx::FloatRect const& rect, Color const& color, CornerRadius const& top_left_radius, CornerRadius const& top_right_radius, CornerRadius const& bottom_left_radius, CornerRadius const& bottom_right_radius, BlendingMode = BlendingMode::AlphaAdd);

    void blit_canvas(Gfx::IntRect const& dst_rect, Canvas const&, float opacity = 1.0f, Optional<Gfx::AffineTransform> affine_transform = {});
    void blit_canvas(Gfx::FloatRect const& dst_rect, Canvas const&, float opacity = 1.0f, Optional<Gfx::AffineTransform> affine_transform = {});
    void blit_canvas(Gfx::FloatRect const& dst_rect, Canvas const&, Gfx::FloatRect const& src_rect, float opacity = 1.0f, Optional<Gfx::AffineTransform> affine_transform = {}, BlendingMode = BlendingMode::AlphaAdd);

    enum class BlurDirection {
        Horizontal,
        Vertical,
    };
    void blit_blurred_canvas(Gfx::FloatRect const& dst_rect, Canvas const&, int radius, BlurDirection direction, ScalingMode = ScalingMode::NearestNeighbor);

    void update_immutable_bitmap_texture_cache(HashMap<u32, Gfx::ImmutableBitmap const*>&);

private:
    Context& m_context;

    struct State {
        Gfx::AffineTransform transform;
        Gfx::IntRect clip_rect;
    };

    [[nodiscard]] State& state() { return m_state_stack.last(); }
    [[nodiscard]] State const& state() const { return m_state_stack.last(); }

    void blit_scaled_texture(Gfx::FloatRect const& dst_rect, GL::Texture const&, Gfx::FloatRect const& src_rect, ScalingMode, float opacity = 1.0f, Optional<Gfx::AffineTransform> affine_transform = {}, BlendingMode = BlendingMode::AlphaAdd);
    void blit_blurred_texture(Gfx::FloatRect const& dst_rect, GL::Texture const&, Gfx::FloatRect const& src_rect, int radius, BlurDirection direction, ScalingMode = ScalingMode::NearestNeighbor);
    void bind_target_canvas();

    [[nodiscard]] Gfx::FloatRect to_clip_space(Gfx::FloatRect const& screen_rect) const;

    Vector<State, 1> m_state_stack;

    NonnullRefPtr<Canvas> m_target_canvas;

    Program m_rectangle_program;
    Program m_rounded_rectangle_program;
    Program m_blit_program;
    Program m_linear_gradient_program;
    Program m_blur_program;
};

}
