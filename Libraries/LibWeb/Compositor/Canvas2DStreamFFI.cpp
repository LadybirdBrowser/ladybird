/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Span.h>
#include <LibCompositing/CanvasColorSpace.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibIPC/ThreadSafeMessagePoster.h>
#include <LibWeb/Compositor/CompositorConnection.h>
#include <LibWeb/HTML/Canvas/PredefinedColorSpace.h>

namespace Web::Compositor {

// NB: host.rs takes the canvas ID as a plain integer.
static_assert(sizeof(Compositing::CanvasId) == sizeof(u64));

// NB: `AlphaType` of host.rs is a u8 with these values.
static_assert(to_underlying(Gfx::AlphaType::Premultiplied) == 0);
static_assert(to_underlying(Gfx::AlphaType::Unpremultiplied) == 1);

extern "C" {

void web_canvas_2d_connection_unref(CompositorConnection*);
bool web_canvas_2d_connection_poster(CompositorConnection*, IPC::FfiThreadSafeMessagePoster* out_poster);
bool web_canvas_2d_connection_reserve_context_id(CompositorConnection*, i32 width, i32 height, u64* out_canvas_id);
bool web_canvas_2d_connection_post_stream_batch_fonts_and_image_frames(CompositorConnection*, Gfx::Font const* const* fonts, size_t font_count, Gfx::DecodedImageFrame const* const* image_frames, size_t image_frame_count);
void web_canvas_2d_connection_did_fail_to_post(CompositorConnection*);
Gfx::DecodedImageFrame* web_canvas_2d_connection_read_back_pixels(CompositorConnection*, u64 canvas_id, i32 x, i32 y, i32 width, i32 height, u8 alpha_type, u8 color_space);
void web_canvas_2d_video_frame_hold_ref(Compositing::VideoFrameHold*);
void web_canvas_2d_video_frame_hold_unref(Compositing::VideoFrameHold*);
void web_canvas_2d_video_frame_hold_note_sent_to_compositor(Compositing::VideoFrameHold*);
}

void web_canvas_2d_connection_unref(CompositorConnection* connection)
{
    connection->unref();
}

bool web_canvas_2d_connection_poster(CompositorConnection* connection, IPC::FfiThreadSafeMessagePoster* out_poster)
{
    auto poster = connection->poster_while_compositor_is_reachable();
    if (!poster)
        return false;
    *out_poster = poster->to_ffi_with_one_reference();
    return true;
}

bool web_canvas_2d_connection_reserve_context_id(CompositorConnection* connection, i32 width, i32 height, u64* out_canvas_id)
{
    auto canvas_id = connection->reserve_canvas_2d_context_id({ width, height });
    if (!canvas_id.has_value())
        return false;
    *out_canvas_id = canvas_id->value();
    return true;
}

bool web_canvas_2d_connection_post_stream_batch_fonts_and_image_frames(CompositorConnection* connection, Gfx::Font const* const* fonts, size_t font_count, Gfx::DecodedImageFrame const* const* image_frames, size_t image_frame_count)
{
    return connection->post_canvas_2d_stream_batch_fonts_and_image_frames({ fonts, font_count }, { image_frames, image_frame_count });
}

void web_canvas_2d_connection_did_fail_to_post(CompositorConnection* connection)
{
    connection->did_fail_to_post_canvas_2d_message();
}

// NB: A color space is the underlying value of a Bindings::PredefinedColorSpace.
// Unpremultiplied pixels come back as RGBA8888 and premultiplied pixels as BGRA8888, in the color space.
Gfx::DecodedImageFrame* web_canvas_2d_connection_read_back_pixels(CompositorConnection* connection, u64 canvas_id, i32 x, i32 y, i32 width, i32 height, u8 alpha_type, u8 color_space)
{
    VERIFY(Gfx::is_valid_alpha_type(alpha_type));
    auto gfx_color_space = Compositing::gfx_color_space_for(HTML::predefined_color_space_from_underlying(color_space));
    auto shareable_bitmap = connection->get_canvas_pixels(Compositing::CanvasId { canvas_id }, { x, y, width, height }, static_cast<Gfx::AlphaType>(alpha_type), gfx_color_space);
    if (!shareable_bitmap.is_valid())
        return nullptr;
    auto frame = Gfx::DecodedImageFrame::create(*shareable_bitmap.bitmap(), gfx_color_space);
    if (frame.is_error())
        return nullptr;
    return new Gfx::DecodedImageFrame { frame.release_value() };
}

void web_canvas_2d_video_frame_hold_ref(Compositing::VideoFrameHold* hold)
{
    hold->ref();
}

void web_canvas_2d_video_frame_hold_unref(Compositing::VideoFrameHold* hold)
{
    hold->unref();
}

void web_canvas_2d_video_frame_hold_note_sent_to_compositor(Compositing::VideoFrameHold* hold)
{
    hold->note_sent_to_compositor();
}

}
