/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <GLES2/gl2.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibIPC/Limits.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLContextProxyBase.h>

namespace Web::WebGL {

WebGLContextProxyBase::WebGLContextProxyBase(NonnullRefPtr<RemoteWebGLTransport> transport, WebGLVersion webgl_version, Vector<String> supported_extensions)
    : m_transport(move(transport))
    , m_webgl_version(webgl_version)
    , m_supported_extensions(move(supported_extensions))
{
}

WebGLContextProxyBase::~WebGLContextProxyBase()
{
    m_transport->destroy_context();
}

void WebGLContextProxyBase::restore(NonnullRefPtr<RemoteWebGLTransport> transport, Vector<String> supported_extensions)
{
    m_transport = move(transport);
    m_supported_extensions = move(supported_extensions);
    m_lost = false;
    m_commands.clear_with_capacity();
    m_pending_bitmaps.clear_with_capacity();
    m_string_cache.clear();
}

void WebGLContextProxyBase::flush_commands()
{
    if (m_commands.is_empty())
        return;
    m_transport->send_commands(m_commands.buffer(), m_pending_bitmaps);
    m_commands.clear_with_capacity();
    m_pending_bitmaps.clear_with_capacity();
}

u32 WebGLContextProxyBase::append_pending_bitmap(Gfx::DecodedImageFrame frame)
{
    static_assert(IPC::MAX_MESSAGE_FD_COUNT > 1);
    // WebGL command bytes are transferred in one anonymous buffer attachment.
    static constexpr size_t max_bitmap_attachments_per_message = IPC::MAX_MESSAGE_FD_COUNT - 1;

    if (m_pending_bitmaps.size() >= max_bitmap_attachments_per_message)
        flush_commands();

    auto bitmap_index = static_cast<u32>(m_pending_bitmaps.size());
    m_pending_bitmaps.append(move(frame));
    return bitmap_index;
}

ByteBuffer WebGLContextProxyBase::send_sync_call(ByteBuffer request)
{
    if (m_lost)
        return {};
    flush_commands();
    auto reply = m_transport->sync_call(move(request));
    if (reply.is_empty())
        set_lost();
    return reply;
}

ReadPixelsResult WebGLContextProxyBase::read_pixels_robust_angle_into_shared_buffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, Core::AnonymousBuffer const& pixels)
{
    flush_commands();
    return m_transport->read_pixels_robust_angle(x, y, width, height, format, type, buf_size, pixels);
}

void WebGLContextProxyBase::set_size(Gfx::IntSize const& size)
{
    record(Commands::SetDrawingBufferSize { .width = size.width(), .height = size.height() });
}

void WebGLContextProxyBase::present_canvas_for_compositing(bool preserve_drawing_buffer)
{
    flush_commands();
    m_transport->present_canvas(preserve_drawing_buffer);
}

RefPtr<Gfx::Bitmap> WebGLContextProxyBase::read_back_drawing_buffer(Gfx::IntRect const& rect)
{
    if (m_lost)
        return nullptr;
    flush_commands();
    auto bitmap = m_transport->read_back_drawing_buffer(rect);
    if (!bitmap.is_valid())
        return nullptr;
    return bitmap.bitmap();
}

void WebGLContextProxyBase::read_pixels_into_pixel_pack_buffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, long long offset)
{
    record(Commands::ReadPixelsIntoPixelPackBuffer {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .format = format,
        .type = type,
        .offset = static_cast<GLintptr>(offset),
    });
}

void WebGLContextProxyBase::tex_image2d_from_bitmap(GLenum target, GLint level, GLint internalformat, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexImage2DFromBitmap {
        .target = target,
        .level = level,
        .internalformat = internalformat,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

void WebGLContextProxyBase::tex_sub_image2d_from_bitmap(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexSubImage2DFromBitmap {
        .target = target,
        .level = level,
        .xoffset = xoffset,
        .yoffset = yoffset,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

void WebGLContextProxyBase::tex_image3d_from_bitmap(GLenum target, GLint level, GLint internalformat, GLsizei depth, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexImage3DFromBitmap {
        .target = target,
        .level = level,
        .internalformat = internalformat,
        .depth = depth,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

void WebGLContextProxyBase::tex_sub_image3d_from_bitmap(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei depth, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexSubImage3DFromBitmap {
        .target = target,
        .level = level,
        .xoffset = xoffset,
        .yoffset = yoffset,
        .zoffset = zoffset,
        .depth = depth,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

bool WebGLContextProxyBase::read_buffer_sub_data(GLenum target, long long offset, Bytes destination)
{
    if (m_lost)
        return false;
    if (destination.is_empty())
        return true;

    auto shared_data_or_error = Core::AnonymousBuffer::create_with_size(destination.size());
    if (shared_data_or_error.is_error()) {
        set_pending_local_error(GL_OUT_OF_MEMORY);
        return false;
    }

    auto shared_data = shared_data_or_error.release_value();
    flush_commands();
    if (!m_transport->read_buffer_sub_data(target, static_cast<GLintptr>(offset), static_cast<GLintptr>(destination.size()), shared_data))
        return false;
    if (m_lost)
        return false;
    __builtin_memcpy(destination.data(), shared_data.data<void>(), destination.size());
    return true;
}

void WebGLContextProxy::shader_source(GLuint shader, GLsizei count, GLchar const* const* string, GLint const* length)
{
    VERIFY(count == 1);
    auto source_length = length ? static_cast<size_t>(length[0]) : __builtin_strlen(string[0]);
    ByteBuffer source_bytes = MUST(ByteBuffer::create_uninitialized(source_length + 1));
    __builtin_memcpy(source_bytes.data(), string[0], source_length);
    source_bytes[source_length] = 0;

    Commands::ShaderSource command { .shader = shader, .source = {} };
    command.source = { WebGLCommandList::first_inline_data_offset(sizeof(command)), static_cast<u32>(source_bytes.size()) };
    record(command, source_bytes);
}

static ByteBuffer pack_strings(GLsizei count, GLchar const* const* strings)
{
    StringBuilder builder;
    for (GLsizei i = 0; i < count; ++i) {
        builder.append({ strings[i], __builtin_strlen(strings[i]) });
        builder.append('\0');
    }
    return MUST(builder.to_byte_buffer());
}

void WebGLContextProxy::transform_feedback_varyings(GLuint program, GLsizei count, GLchar const* const* varyings, GLenum bufferMode)
{
    auto varyings_bytes = pack_strings(count, varyings);
    Commands::TransformFeedbackVaryings command { .program = program, .count = count, .varyings = {}, .buffer_mode = bufferMode };
    command.varyings = { WebGLCommandList::first_inline_data_offset(sizeof(command)), static_cast<u32>(varyings_bytes.size()) };
    record(command, varyings_bytes);
}

void WebGLContextProxy::read_pixels_robust_angle(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels)
{
    if (is_lost())
        return;

    Core::AnonymousBuffer shared_pixels;
    if (bufSize > 0) {
        auto shared_pixels_or_error = Core::AnonymousBuffer::create_with_size(static_cast<size_t>(bufSize));
        if (shared_pixels_or_error.is_error()) {
            set_pending_local_error(GL_OUT_OF_MEMORY);
            return;
        }
        shared_pixels = shared_pixels_or_error.release_value();
    }

    auto result = read_pixels_robust_angle_into_shared_buffer(x, y, width, height, format, type, bufSize, shared_pixels);
    if (is_lost())
        return;
    if (length)
        *length = result.length;
    if (columns)
        *columns = result.columns;
    if (rows)
        *rows = result.rows;
    if (pixels && result.length > 0) {
        VERIFY(result.length <= bufSize);
        __builtin_memcpy(pixels, shared_pixels.data<void>(), static_cast<size_t>(result.length));
    }
}

GLubyte const* WebGLContextProxy::get_string(GLenum name)
{
    if (auto cached = m_string_cache.get(name); cached.has_value())
        return cached.value()->data();

    SyncCalls::GetString::Request request { .name = name };
    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<SyncCalls::GetString>(request));
    if (is_lost())
        return reinterpret_cast<GLubyte const*>("");
    auto reply = WebGLSyncCall::decode_reply<SyncCalls::GetString::Reply>(reply_bytes);
    auto resolved = WebGLCommandList::resolve_string_span(reply_bytes, reply.value);
    auto value = make<ByteBuffer>(MUST(ByteBuffer::copy(resolved)));
    auto const* data = value->data();
    m_string_cache.set(name, move(value));
    return data;
}

void WebGLContextProxy::get_vertex_attrib_pointerv_robust_angle(GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, void** pointer)
{
    (void)bufSize;
    SyncCalls::GetVertexAttribPointervRobustANGLE::Request request { .index = index, .pname = pname };
    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<SyncCalls::GetVertexAttribPointervRobustANGLE>(request));
    if (is_lost())
        return;
    auto reply = WebGLSyncCall::decode_reply<SyncCalls::GetVertexAttribPointervRobustANGLE::Reply>(reply_bytes);
    if (length)
        *length = 1;
    if (pointer)
        *pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(reply.pointer));
}

void WebGLContextProxy::get_uniform_indices(GLuint program, GLsizei uniformCount, GLchar const* const* uniformNames, GLuint* uniformIndices)
{
    auto names_bytes = pack_strings(uniformCount, uniformNames);
    SyncCalls::GetUniformIndices::Request request { .program = program, .uniform_count = uniformCount, .uniform_names = {} };
    request.uniform_names = { WebGLCommandList::first_inline_data_offset(sizeof(request)), static_cast<u32>(names_bytes.size()) };
    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<SyncCalls::GetUniformIndices>(request, names_bytes));
    if (is_lost())
        return;
    auto reply = WebGLSyncCall::decode_reply<SyncCalls::GetUniformIndices::Reply>(reply_bytes);
    if (uniformIndices)
        WebGLCommandList::copy_data_span(reply_bytes, reply.uniform_indices, { uniformIndices, static_cast<size_t>(uniformCount) * sizeof(GLuint) });
}

void* WebGLContextProxy::map_buffer_range(GLenum, GLintptr, GLsizeiptr, GLbitfield)
{
    // getBufferSubData() goes through read_buffer_sub_data() instead; nothing else maps.
    VERIFY_NOT_REACHED();
}

GLboolean WebGLContextProxy::unmap_buffer(GLenum)
{
    VERIFY_NOT_REACHED();
}

}
