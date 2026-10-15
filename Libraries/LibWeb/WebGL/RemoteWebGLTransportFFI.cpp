/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCompositing/WebGL/WebGLCommandList.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Limits.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>

// The C ABI of RemoteWebGLTransport, for the WebGL command recorder in Libraries/LibWeb/Bindings/Rust/src/web_gl,
// which declares these functions in host.rs.
//
// The recorder holds a transport and a bitmap as a pointer that owns one reference, and an anonymous buffer and an
// image frame as a pointer to an AnonymousBufferOwnedByRecorder or a Gfx::DecodedImageFrame that it owns.

namespace Web::WebGL {

// NB: `ReadPixelsResult` of proxy/transport.rs is three `i32`s in this order.
static_assert(sizeof(ReadPixelsResult) == 3 * sizeof(i32));
static_assert(offsetof(ReadPixelsResult, length) == 0);
static_assert(offsetof(ReadPixelsResult, columns) == sizeof(i32));
static_assert(offsetof(ReadPixelsResult, rows) == 2 * sizeof(i32));
static_assert(sizeof(Compositing::CanvasId) == sizeof(u64));

// What the WebGL implementation in Rust states for itself besides the command structs: the framing and the shared
// data region that the recorder and the replayer of the command stream have to agree on, and the largest drawing
// buffer that the compositor allocates. Rust compares its own constants with these before the first context records
// (fingerprint.rs).
struct ConstantsRestatedInRust {
    size_t record_alignment { 0 };
    size_t header_size { 0 };
    size_t header_payload_size_offset { 0 };
    size_t shared_data_region_offset { 0 };
    size_t shared_data_region_capacity { 0 };
    size_t max_drawing_buffer_dimension { 0 };
};

// NB: The recorder writes the header of a command and that of a synchronous call the same way.
static_assert(sizeof(Compositing::WebGL::WebGLSyncCallHeader) == sizeof(Compositing::WebGL::WebGLCommandHeader));
static_assert(offsetof(Compositing::WebGL::WebGLSyncCallHeader, payload_size) == offsetof(Compositing::WebGL::WebGLCommandHeader, payload_size));
static_assert(offsetof(Compositing::WebGL::WebGLSyncCallHeader, type) == 0 && offsetof(Compositing::WebGL::WebGLCommandHeader, type) == 0);
static_assert(sizeof(Compositing::WebGL::WebGLSyncCallType) == sizeof(u16) && sizeof(Compositing::WebGL::WebGLCommandType) == sizeof(u16));
static_assert(sizeof(Compositing::WebGL::WebGLCommandHeader::payload_size) == sizeof(u32));

// NB: `DataSpan` of proxy/wire.rs is two `u32`s in this order.
static_assert(sizeof(Compositing::WebGL::WebGLDataSpan) == 2 * sizeof(u32));
static_assert(offsetof(Compositing::WebGL::WebGLDataSpan, offset) == 0);
static_assert(offsetof(Compositing::WebGL::WebGLDataSpan, size) == sizeof(u32));

extern "C" {

void web_webgl_transport_unref(void* transport);
bool web_webgl_transport_canvas_id(void const* transport, u64* out_canvas_id);
void web_webgl_transport_destroy_context(void* transport);
void web_webgl_transport_set_shared_command_buffer(void* transport, void const* anonymous_buffer);
void web_webgl_transport_send_commands_from_shared_buffer(void* transport, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, void const* const* frames, size_t frame_count);
bool web_webgl_transport_wait_until_published_commands_executed(void* transport);
void web_webgl_transport_send_commands(void* transport, u8 const* commands, size_t size_in_bytes, void const* const* frames, size_t frame_count);
void web_webgl_transport_present_canvas(void* transport, bool preserve_drawing_buffer);
void web_webgl_transport_clear_drawing_buffer(void* transport);
void web_webgl_transport_sync_call(void* transport, u8 const* request, size_t request_size, void* reply_sink, void (*write_reply)(void* reply_sink, u8 const* reply, size_t reply_size));
void web_webgl_transport_read_pixels_robust_angle(void* transport, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, void const* pixels_anonymous_buffer_or_null, ReadPixelsResult* out_result);
bool web_webgl_transport_read_buffer_sub_data(void* transport, u32 target, i64 offset, i64 size, void const* data_anonymous_buffer);
void* web_webgl_transport_read_back_drawing_buffer(void* transport, i32 x, i32 y, i32 width, i32 height);
size_t web_webgl_max_frames_per_message();
u8 web_navigator_compatibility_mode();
void web_webgl_constants_restated_in_rust(ConstantsRestatedInRust* out_constants);
void* web_webgl_anonymous_buffer_create(size_t size, u8** out_data);
void web_webgl_anonymous_buffer_destroy(void* anonymous_buffer);
void web_webgl_frame_destroy(void* frame);
void web_webgl_bitmap_unref(void* bitmap);
}

static RemoteWebGLTransport& transport_from(void* transport)
{
    return *static_cast<RemoteWebGLTransport*>(transport);
}

// NB: Core::AnonymousBuffer cannot be allocated with kmalloc itself: a class that derives from it and from a
//     ref-counted base would then have two operator new.
struct AnonymousBufferOwnedByRecorder {
    AK_ALLOC_WITH_KMALLOC;

    Core::AnonymousBuffer buffer;
};

static Core::AnonymousBuffer const& anonymous_buffer_from(void const* anonymous_buffer)
{
    return static_cast<AnonymousBufferOwnedByRecorder const*>(anonymous_buffer)->buffer;
}

static Vector<Gfx::DecodedImageFrame> copy_frames(void const* const* frames, size_t frame_count)
{
    Vector<Gfx::DecodedImageFrame> copied_frames;
    copied_frames.ensure_capacity(frame_count);
    for (size_t i = 0; i < frame_count; ++i)
        copied_frames.unchecked_append(*static_cast<Gfx::DecodedImageFrame const*>(frames[i]));
    return copied_frames;
}

void web_webgl_transport_unref(void* transport)
{
    transport_from(transport).unref();
}

bool web_webgl_transport_canvas_id(void const* transport, u64* out_canvas_id)
{
    auto canvas_id = static_cast<RemoteWebGLTransport const*>(transport)->canvas_id();
    if (!canvas_id.has_value())
        return false;
    *out_canvas_id = canvas_id->value();
    return true;
}

void web_webgl_transport_destroy_context(void* transport)
{
    transport_from(transport).destroy_context();
}

void web_webgl_transport_set_shared_command_buffer(void* transport, void const* anonymous_buffer)
{
    transport_from(transport).set_shared_command_buffer(anonymous_buffer_from(anonymous_buffer));
}

void web_webgl_transport_send_commands_from_shared_buffer(void* transport, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, void const* const* frames, size_t frame_count)
{
    transport_from(transport).send_commands_from_shared_buffer(offset, size_in_bytes, flush_sequence_number, copy_frames(frames, frame_count));
}

bool web_webgl_transport_wait_until_published_commands_executed(void* transport)
{
    return transport_from(transport).wait_until_published_commands_executed();
}

void web_webgl_transport_send_commands(void* transport, u8 const* commands, size_t size_in_bytes, void const* const* frames, size_t frame_count)
{
    transport_from(transport).send_commands({ commands, size_in_bytes }, copy_frames(frames, frame_count));
}

void web_webgl_transport_present_canvas(void* transport, bool preserve_drawing_buffer)
{
    transport_from(transport).present_canvas(preserve_drawing_buffer);
}

void web_webgl_transport_clear_drawing_buffer(void* transport)
{
    transport_from(transport).clear_drawing_buffer();
}

void web_webgl_transport_sync_call(void* transport, u8 const* request, size_t request_size, void* reply_sink, void (*write_reply)(void* reply_sink, u8 const* reply, size_t reply_size))
{
    auto reply = transport_from(transport).sync_call(MUST(ByteBuffer::copy(request, request_size)));
    write_reply(reply_sink, reply.data(), reply.size());
}

void web_webgl_transport_read_pixels_robust_angle(void* transport, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, void const* pixels_anonymous_buffer_or_null, ReadPixelsResult* out_result)
{
    Core::AnonymousBuffer pixels;
    if (pixels_anonymous_buffer_or_null)
        pixels = anonymous_buffer_from(pixels_anonymous_buffer_or_null);
    *out_result = transport_from(transport).read_pixels_robust_angle(x, y, width, height, format, type, buf_size, move(pixels));
}

bool web_webgl_transport_read_buffer_sub_data(void* transport, u32 target, i64 offset, i64 size, void const* data_anonymous_buffer)
{
    return transport_from(transport).read_buffer_sub_data(target, offset, size, anonymous_buffer_from(data_anonymous_buffer));
}

void* web_webgl_transport_read_back_drawing_buffer(void* transport, i32 x, i32 y, i32 width, i32 height)
{
    auto shareable_bitmap = transport_from(transport).read_back_drawing_buffer({ x, y, width, height });
    if (!shareable_bitmap.is_valid())
        return nullptr;
    return &NonnullRefPtr<Gfx::Bitmap> { *shareable_bitmap.bitmap() }.leak_ref();
}

size_t web_webgl_max_frames_per_message()
{
    // NB: The bytes of the commands take one of the attachments of a message.
    static_assert(IPC::MAX_MESSAGE_FD_COUNT > 1);
    return IPC::MAX_MESSAGE_FD_COUNT - 1;
}

// The navigator compatibility mode, numbered as Web::NavigatorCompatibilityMode is: Chrome, Gecko, WebKit.
u8 web_navigator_compatibility_mode()
{
    return to_underlying(Web::ResourceLoader::navigator_compatibility_mode());
}

void web_webgl_constants_restated_in_rust(ConstantsRestatedInRust* out_constants)
{
    *out_constants = {
        .record_alignment = Compositing::WebGL::WebGLCommandList::command_alignment,
        .header_size = sizeof(Compositing::WebGL::WebGLCommandHeader),
        .header_payload_size_offset = offsetof(Compositing::WebGL::WebGLCommandHeader, payload_size),
        .shared_data_region_offset = Compositing::WebGL::WebGLSharedCommandBuffer::data_region_offset,
        .shared_data_region_capacity = Compositing::WebGL::WebGLSharedCommandBuffer::default_data_region_capacity,
        .max_drawing_buffer_dimension = Compositing::WebGL::max_webgl_drawing_buffer_dimension,
    };
}

void* web_webgl_anonymous_buffer_create(size_t size, u8** out_data)
{
    auto buffer_or_error = Core::AnonymousBuffer::create_with_size(size);
    if (buffer_or_error.is_error())
        return nullptr;
    auto* owned_buffer = new AnonymousBufferOwnedByRecorder { buffer_or_error.release_value() };
    *out_data = owned_buffer->buffer.data<u8>();
    return owned_buffer;
}

void web_webgl_anonymous_buffer_destroy(void* anonymous_buffer)
{
    delete static_cast<AnonymousBufferOwnedByRecorder*>(anonymous_buffer);
}

void web_webgl_frame_destroy(void* frame)
{
    delete static_cast<Gfx::DecodedImageFrame*>(frame);
}

void web_webgl_bitmap_unref(void* bitmap)
{
    static_cast<Gfx::Bitmap*>(bitmap)->unref();
}

}
