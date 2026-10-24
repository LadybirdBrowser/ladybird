/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Span.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibWeb/HTML/Canvas/PredefinedColorSpace.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>

// The C ABI of RemoteCanvas2DTransport, for the 2D canvas command recorder in Libraries/LibWeb/Bindings/Rust/src/canvas_2d,
// which declares these functions in host.rs.
//
// The recorder holds a transport and a bitmap as a pointer that owns one reference. A font, a decoded image frame and a
// video frame hold are lent for the call that takes them. The pixels of a bitmap that is read back are read in place.

namespace Web::HTML {

// NB: host.rs passes a record as an ak::Span, which is laid out as a ReadonlyBytes, and takes the ids of the resources,
//     the canvas ID and the pending command count as plain integers.
static_assert(sizeof(ReadonlyBytes) == sizeof(u8 const*) + sizeof(size_t));
static_assert(sizeof(Compositing::CanvasId) == sizeof(u64));
static_assert(sizeof(Compositing::FontResourceId) == sizeof(u64));
static_assert(sizeof(Compositing::ImageFrameResourceId) == sizeof(u64));

// NB: `AlphaType` of host.rs is a u8 with these values.
static_assert(to_underlying(Gfx::AlphaType::Premultiplied) == 0);
static_assert(to_underlying(Gfx::AlphaType::Unpremultiplied) == 1);

extern "C" {

void web_canvas_2d_transport_unref(void* transport);
bool web_canvas_2d_transport_create_context(void* transport, i32 width, i32 height, bool alpha, bool will_read_frequently, u8 color_space);
bool web_canvas_2d_transport_canvas_id(void const* transport, u64* out_canvas_id);
void web_canvas_2d_transport_destroy_context(void* transport);
void web_canvas_2d_transport_append_command(void* transport, ReadonlyBytes record);
u64 web_canvas_2d_transport_add_font(void* transport, void const* font);
u64 web_canvas_2d_transport_add_image_frame(void* transport, void const* frame);
void web_canvas_2d_transport_retain_video_frame_hold(void* transport, void* hold);
void web_canvas_2d_transport_record_present(void* transport);
size_t web_canvas_2d_transport_pending_command_count(void const* transport);
void web_canvas_2d_transport_flush(void* transport);
void* web_canvas_2d_transport_read_back_pixels(void* transport, i32 x, i32 y, i32 width, i32 height, u8 alpha_type, u8 color_space);
void web_canvas_2d_bitmap_ref(void* bitmap);
void web_canvas_2d_bitmap_unref(void* bitmap);
size_t web_canvas_2d_bitmap_size_in_bytes(void* bitmap);
bool web_canvas_2d_bitmap_unpremultiplied_rgba_pixels(void* bitmap, u8 const** out_pixels, size_t* out_pitch, i32* out_width, i32* out_height);
void* web_canvas_2d_image_frame_from_bitmap(void* bitmap);
}

static RemoteCanvas2DTransport& transport_from(void* transport)
{
    return *static_cast<RemoteCanvas2DTransport*>(transport);
}

void web_canvas_2d_transport_unref(void* transport)
{
    transport_from(transport).unref();
}

// NB: A color space is the underlying value of a Bindings::PredefinedColorSpace.
bool web_canvas_2d_transport_create_context(void* transport, i32 width, i32 height, bool alpha, bool will_read_frequently, u8 color_space)
{
    return transport_from(transport).create_context({ width, height }, alpha, will_read_frequently, gfx_color_space_for(predefined_color_space_from_underlying(color_space)));
}

bool web_canvas_2d_transport_canvas_id(void const* transport, u64* out_canvas_id)
{
    auto canvas_id = static_cast<RemoteCanvas2DTransport const*>(transport)->canvas_id();
    if (!canvas_id.has_value())
        return false;
    *out_canvas_id = canvas_id->value();
    return true;
}

void web_canvas_2d_transport_destroy_context(void* transport)
{
    transport_from(transport).destroy_context();
}

void web_canvas_2d_transport_append_command(void* transport, ReadonlyBytes record)
{
    transport_from(transport).append_command(record);
}

u64 web_canvas_2d_transport_add_font(void* transport, void const* font)
{
    return transport_from(transport).add_font(*static_cast<Gfx::Font const*>(font)).value();
}

u64 web_canvas_2d_transport_add_image_frame(void* transport, void const* frame)
{
    return transport_from(transport).add_image_frame(*static_cast<Gfx::DecodedImageFrame const*>(frame)).value();
}

void web_canvas_2d_transport_retain_video_frame_hold(void* transport, void* hold)
{
    transport_from(transport).retain_video_frame_hold(*static_cast<Compositing::VideoFrameHold*>(hold));
}

void web_canvas_2d_transport_record_present(void* transport)
{
    transport_from(transport).record_present();
}

size_t web_canvas_2d_transport_pending_command_count(void const* transport)
{
    return static_cast<RemoteCanvas2DTransport const*>(transport)->pending_command_count();
}

void web_canvas_2d_transport_flush(void* transport)
{
    transport_from(transport).flush();
}

void* web_canvas_2d_transport_read_back_pixels(void* transport, i32 x, i32 y, i32 width, i32 height, u8 alpha_type, u8 color_space)
{
    VERIFY(Gfx::is_valid_alpha_type(alpha_type));
    auto bitmap = transport_from(transport).read_back_pixels({ x, y, width, height }, static_cast<Gfx::AlphaType>(alpha_type), gfx_color_space_for(predefined_color_space_from_underlying(color_space)));
    if (!bitmap)
        return nullptr;
    return &bitmap.release_nonnull().leak_ref();
}

void web_canvas_2d_bitmap_ref(void* bitmap)
{
    static_cast<Gfx::Bitmap*>(bitmap)->ref();
}

void web_canvas_2d_bitmap_unref(void* bitmap)
{
    static_cast<Gfx::Bitmap*>(bitmap)->unref();
}

size_t web_canvas_2d_bitmap_size_in_bytes(void* bitmap)
{
    return static_cast<Gfx::Bitmap*>(bitmap)->size_in_bytes();
}

bool web_canvas_2d_bitmap_unpremultiplied_rgba_pixels(void* bitmap, u8 const** out_pixels, size_t* out_pitch, i32* out_width, i32* out_height)
{
    auto const& typed_bitmap = *static_cast<Gfx::Bitmap const*>(bitmap);
    if (typed_bitmap.format() != Gfx::BitmapFormat::RGBA8888 || typed_bitmap.alpha_type() != Gfx::AlphaType::Unpremultiplied)
        return false;
    *out_pixels = typed_bitmap.scanline_u8(0);
    *out_pitch = typed_bitmap.pitch();
    *out_width = typed_bitmap.width();
    *out_height = typed_bitmap.height();
    return true;
}

// A Gfx::DecodedImageFrame of the bitmap, allocated with new, which takes over the reference to the bitmap.
void* web_canvas_2d_image_frame_from_bitmap(void* bitmap)
{
    NonnullRefPtr<Gfx::Bitmap const> adopted_bitmap = adopt_ref(*static_cast<Gfx::Bitmap*>(bitmap));
    return new Gfx::DecodedImageFrame { move(adopted_bitmap) };
}

}
