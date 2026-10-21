/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Path.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Forward.h>

namespace Gfx {

class Filter;
class PaintStyle;

}

namespace Web::HTML {

struct RustCanvas2DRecorder;

// How a draw composites onto the canvas.
struct Canvas2DCompositing {
    Gfx::Filter const* filter { nullptr };
    float global_alpha { 1 };
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator { Gfx::CompositingAndBlendingOperator::SourceOver };
};

struct Canvas2DStroke {
    float thickness { 1 };
    Gfx::Path::CapStyle cap_style { Gfx::Path::CapStyle::Butt };
    Gfx::Path::JoinStyle join_style { Gfx::Path::JoinStyle::Miter };
    float miter_limit { 10 };
    ReadonlySpan<float> dash_array;
    float dash_offset { 0 };
};

// The recorder of a 2D canvas, which Libraries/LibWeb/Bindings/Rust/src/canvas_2d/exports.rs implements: it writes the
// commands of the canvas as records of the display list tape into the stream that the canvases of WebContent share,
// and reaches the Compositor through the transport that it owns.
class Canvas2DRecorder {
    AK_MAKE_NONCOPYABLE(Canvas2DRecorder);
    AK_MAKE_NONMOVABLE(Canvas2DRecorder);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<Canvas2DRecorder> create(NonnullRefPtr<RemoteCanvas2DTransport>);
    ~Canvas2DRecorder();

    bool create_context(Gfx::IntSize, bool alpha);
    Optional<Compositing::CanvasId> canvas_id() const;
    void destroy_context();
    void flush();
    // Unpremultiplied pixels come back as RGBA8888 and premultiplied pixels as BGRA8888.
    RefPtr<Gfx::Bitmap> read_back_pixels(Gfx::IntRect const&, Gfx::AlphaType);
    void present();
    // Sends the shared stream once it holds a batch, so that what is recorded next travels with its resources.
    void prepare_to_record();

    void fill_path(Gfx::Path const&, Gfx::PaintStyle const&, Gfx::WindingRule, Canvas2DCompositing const&);
    // A shadow: a fill in a solid color, blurred by the blur radius.
    void fill_path_shadow(Gfx::Path const&, Gfx::Color, Gfx::WindingRule, float blur_radius, Canvas2DCompositing const&);
    void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, Canvas2DStroke const&, Canvas2DCompositing const&);
    void stroke_path_shadow(Gfx::Path const&, Gfx::Color, Canvas2DStroke const&, float blur_radius, Canvas2DCompositing const&);
    void draw_glyph_run(Gfx::Font const&, ReadonlySpan<Compositing::DisplayListGlyph>, Gfx::FloatPoint translation, Gfx::PaintStyle const&, Canvas2DCompositing const&);
    void clear_rect(Gfx::FloatRect const&, Gfx::Color);
    void draw_image_frame(Gfx::DecodedImageFrame const&, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode, Canvas2DCompositing const&);
    void draw_canvas(Compositing::CanvasId source_canvas_id, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode, Canvas2DCompositing const&);
    void draw_video_frame(Compositing::CapturedVideoFrame const&, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode, Canvas2DCompositing const&);
    void set_transform(Gfx::AffineTransform const&);
    void save();
    void restore();
    void clip_path(Gfx::Path const&, Gfx::WindingRule);
    void reset();
    void clear_canvas(Gfx::Color);

private:
    explicit Canvas2DRecorder(RustCanvas2DRecorder*);

    RustCanvas2DRecorder* m_recorder { nullptr };
};

}
