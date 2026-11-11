/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>

class SkColorSpace;
class SkImage;
class SkImageFilter;
class SkPaint;
class SkPath;
template<typename T>
class sk_sp;

namespace Compositor {

// Plays the tape of a 2D canvas into the surface of the canvas.
class Canvas2DCommandPlayer {
    AK_MAKE_NONCOPYABLE(Canvas2DCommandPlayer);
    AK_MAKE_NONMOVABLE(Canvas2DCommandPlayer);

public:
    AK_ALLOC_WITH_KMALLOC;

    using FontResolver = Function<Gfx::Font const*(u64)>;
    using ImageFrameResolver = Function<Gfx::DecodedImageFrame const*(u64)>;
    using CanvasSurfaceResolver = Function<Gfx::PaintingSurface const*(u64)>;
    using VideoFrameImageResolver = Function<sk_sp<SkImage>(u64 video_sink_handle, u64 generation)>;

    struct Resolvers {
        CanvasSurfaceResolver canvas_surface;
        FontResolver font;
        ImageFrameResolver image_frame;
        VideoFrameImageResolver video_frame_image;
    };

    // The pixels of the surface, and the colors of the commands, are in the color space. Fails when a surface of the
    // size can't be allocated.
    static ErrorOr<NonnullOwnPtr<Canvas2DCommandPlayer>> create(RefPtr<Gfx::SkiaBackendContext>, Gfx::IntSize, Gfx::BitmapFormat, Gfx::AlphaType, Gfx::ColorSpace const&, Resolvers);
    Canvas2DCommandPlayer(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::ColorSpace const&, Resolvers);
    ~Canvas2DCommandPlayer();

    NonnullRefPtr<Gfx::PaintingSurface> surface() const;

    void clear(Gfx::Color);

    // Plays a tape that passed Compositing::validate_canvas_2d_tape().
    void play(ReadonlyBytes tape);

private:
#define DECLARE_PLAY_COMMAND(command_type) \
    void play_command(Compositing::command_type const&);
    ENUMERATE_CANVAS_2D_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND

    void clear_rect(Gfx::FloatRect const&, Gfx::Color);
    sk_sp<SkImage> image_of_frame(Gfx::DecodedImageFrame const&) const;
    void draw_image(SkImage const&, Gfx::FloatRect const& dst_rect, Gfx::FloatRect const& src_rect, Gfx::ScalingMode, Compositing::Canvas2DCompositing const&);

    ReadonlyBytes inline_data(Compositing::DisplayListDataSpan) const;
    SkPath path_from(Compositing::DisplayListDataSpan) const;
    sk_sp<SkImageFilter> image_filter_from(Compositing::Canvas2DCompositing const&) const;
    // The colors of a canvas are in the color space of its surface.
    SkColorSpace* surface_color_space() const;
    // Has no value if the image of a pattern can't be resolved.
    Optional<SkPaint> paint_from(Compositing::Canvas2DPaint const& style, Compositing::Canvas2DCompositing const&) const;

    Gfx::ColorSpace m_color_space;
    NonnullRefPtr<Gfx::PaintingSurface> m_surface;
    int m_initial_save_count { 0 };
    Resolvers m_resolvers;
    // The payload of the record that plays, which the spans of its command address.
    ReadonlyBytes m_payload;
};

}
