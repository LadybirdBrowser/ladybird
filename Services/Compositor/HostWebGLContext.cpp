/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <Compositor/HostWebGLContext.h>
#include <Compositor/WebGLCommandReplayer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapExport.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/WebGL/TextureUpload.h>
#include <LibWeb/WebGL/WebGLCommandList.h>

namespace Compositor {

using namespace Web::WebGL;

static constexpr GLsizei max_webgl_string_list_entries = 16384;

HostWebGLContext::HostWebGLContext(NonnullOwnPtr<OpenGLContext> gl_context)
    : m_gl_context(move(gl_context))
{
}

OwnPtr<HostWebGLContext> HostWebGLContext::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, OpenGLContext::WebGLVersion version, OpenGLContext::DrawingBufferOptions options, Gfx::IntSize initial_size)
{
    if (initial_size.width() < 1 || initial_size.width() > max_webgl_drawing_buffer_dimension
        || initial_size.height() < 1 || initial_size.height() > max_webgl_drawing_buffer_dimension)
        return {};
    auto gl_context = OpenGLContext::create(skia_backend_context, version, options);
    if (!gl_context)
        return {};
    gl_context->set_size(initial_size);
    return adopt_own(*new HostWebGLContext(gl_context.release_nonnull()));
}

ErrorOr<void> HostWebGLContext::execute_commands(ReadonlyBytes bytes, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    m_gl_context->make_current();

    // A non-preserving context's drawing buffer is cleared after being prepared for
    // compositing, but the clear is deferred to here (the start of the next frame's
    // commands) so a readback taken before then still sees the rendered frame.
    if (m_needs_clear_before_next_frame) {
        m_gl_context->clear_buffer_to_default_values();
        m_needs_clear_before_next_frame = false;
    }

    return WebGLCommandList::for_each_command(bytes, [&]<typename Command>(Command const& command, [[maybe_unused]] ReadonlyBytes payload) -> ErrorOr<void> {
        if constexpr (IsSame<Command, Commands::SetDrawingBufferSize>) {
            return set_drawing_buffer_size(command.width, command.height);
        } else if constexpr (IsSame<Command, Commands::ReadPixelsIntoPixelPackBuffer>) {
            m_gl_context->read_pixels_robust_angle(command.x, command.y, command.width, command.height, command.format, command.type, 0, nullptr, nullptr, nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(command.offset)));
            return {};
        } else if constexpr (IsSame<Command, Commands::TexImage2DFromBitmap>) {
            return tex_image2d_from_bitmap(command, bitmaps);
        } else if constexpr (IsSame<Command, Commands::TexSubImage2DFromBitmap>) {
            return tex_sub_image2d_from_bitmap(command, bitmaps);
        } else {
            return replay_webgl_command(*m_gl_context, m_objects, command, payload);
        }
    });
}

static ErrorOr<Gfx::BitmapExportResult> convert_bitmap_for_upload(Vector<Gfx::DecodedImageFrame> const& bitmaps, u32 bitmap_index, GLenum format, GLenum type, bool has_explicit_destination_size, GLsizei destination_width, GLsizei destination_height, bool flip_y, bool premultiply_alpha)
{
    if (bitmap_index >= bitmaps.size())
        return Error::from_string_literal("WebGL image upload references an out-of-range bitmap");

    auto export_format = texture_export_format(format, type);
    if (!export_format.has_value())
        return Error::from_string_literal("WebGL image upload has an unsupported format+type combination");

    int export_flags = 0;
    if (flip_y)
        export_flags |= Gfx::ExportFlags::FlipY;
    if (premultiply_alpha)
        export_flags |= Gfx::ExportFlags::PremultiplyAlpha;

    Optional<int> target_width;
    Optional<int> target_height;
    if (has_explicit_destination_size) {
        if (destination_width < 0 || destination_height < 0) {
            return Gfx::BitmapExportResult {
                .buffer = {},
                .width = destination_width,
                .height = destination_height,
            };
        }
        target_width = destination_width;
        target_height = destination_height;
    }

    auto const& frame = bitmaps[bitmap_index];
    return Gfx::export_bitmap_to_byte_buffer(frame.bitmap(), frame.color_space(), export_format.value(), export_flags, target_width, target_height);
}

ErrorOr<void> HostWebGLContext::tex_image2d_from_bitmap(Commands::TexImage2DFromBitmap const& command, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    auto converted = TRY(convert_bitmap_for_upload(bitmaps, command.bitmap_index, command.format, command.type, command.has_explicit_destination_size, command.destination_width, command.destination_height, command.flip_y, command.premultiply_alpha));
    m_gl_context->tex_image2d_robust_angle(command.target, command.level, command.internalformat, converted.width, converted.height, 0, command.format, command.type, converted.buffer.size(), converted.buffer.data());
    return {};
}

ErrorOr<void> HostWebGLContext::tex_sub_image2d_from_bitmap(Commands::TexSubImage2DFromBitmap const& command, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    auto converted = TRY(convert_bitmap_for_upload(bitmaps, command.bitmap_index, command.format, command.type, command.has_explicit_destination_size, command.destination_width, command.destination_height, command.flip_y, command.premultiply_alpha));
    m_gl_context->tex_sub_image2d_robust_angle(command.target, command.level, command.xoffset, command.yoffset, converted.width, converted.height, command.format, command.type, converted.buffer.size(), converted.buffer.data());
    return {};
}

ErrorOr<ByteBuffer> HostWebGLContext::execute_sync_call(ReadonlyBytes request)
{
    m_gl_context->make_current();
    return handle_webgl_sync_call(*m_gl_context, m_objects, request);
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> HostWebGLContext::prepare_for_compositing(bool preserve_drawing_buffer)
{
    // Flush all pending GL work so Skia samples the finished drawing buffer. The
    // default framebuffer was written behind Skia's back, so discard cached snapshots
    // before the display-list player asks Skia for an image.
    m_gl_context->present(/* preserve_drawing_buffer= */ true);

    auto drawing_surface = m_gl_context->surface();
    if (!drawing_surface)
        return Error::from_string_literal("WebGL context has no drawing buffer");
    m_gl_context->notify_content_will_change();

    // Defer the clear (see execute_commands) so a readback before the next frame still sees
    // this frame.
    if (!preserve_drawing_buffer)
        m_needs_clear_before_next_frame = true;

    return drawing_surface.release_nonnull();
}

RefPtr<Gfx::PaintingSurface> HostWebGLContext::surface()
{
    return m_gl_context->surface();
}

Gfx::ShareableBitmap HostWebGLContext::read_back_drawing_buffer(Gfx::IntRect rect)
{
    m_gl_context->make_current();
    m_gl_context->present(/* preserve_drawing_buffer= */ true);
    auto surface = m_gl_context->surface();
    if (!surface)
        return {};

    auto clipped_rect = rect.intersected(surface->rect());
    if (clipped_rect.is_empty())
        return {};

    auto bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, clipped_rect.size());
    if (bitmap_or_error.is_error())
        return {};
    auto bitmap = bitmap_or_error.release_value();
    surface->flush();
    surface->read_into_bitmap(*bitmap, clipped_rect.location());
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

ReadPixelsResult HostWebGLContext::read_pixels_robust_angle(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, Core::AnonymousBuffer pixels)
{
    VERIFY(buf_size >= 0);
    VERIFY(static_cast<size_t>(buf_size) <= pixels.size());

    m_gl_context->make_current();

    GLsizei length = 0;
    GLsizei columns = 0;
    GLsizei rows = 0;
    m_gl_context->read_pixels_robust_angle(x, y, width, height, format, type, buf_size, &length, &columns, &rows, pixels.data<void>());
    return {
        .length = length,
        .columns = columns,
        .rows = rows,
    };
}

void HostWebGLContext::read_buffer_sub_data(GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer data)
{
    VERIFY(size >= 0);
    VERIFY(static_cast<size_t>(size) <= data.size());

    m_gl_context->make_current();

    if (auto* mapped = m_gl_context->map_buffer_range(target, offset, size, GL_MAP_READ_BIT)) {
        __builtin_memcpy(data.data<void>(), mapped, static_cast<size_t>(size));
        m_gl_context->unmap_buffer(target);
    }
}

ErrorOr<void> HostWebGLContext::set_drawing_buffer_size(int width, int height)
{
    VERIFY(width >= 1);
    VERIFY(width <= max_webgl_drawing_buffer_dimension);
    VERIFY(height >= 1);
    VERIFY(height <= max_webgl_drawing_buffer_dimension);
    m_gl_context->set_size({ width, height });
    m_gl_context->make_current();
    return {};
}

ErrorOr<void> replay_webgl_command(OpenGLContext& gl, WebGLObjectMap& objects, Commands::ShaderSource const& command, ReadonlyBytes payload)
{
    auto source_bytes = WebGLCommandList::resolve_string_span(payload, command.source);
    auto shader = objects.lookup(command.shader);
    GLchar const* source = reinterpret_cast<GLchar const*>(source_bytes.data());
    GLint length = static_cast<GLint>(source_bytes.size() - 1);
    gl.shader_source(shader, 1, &source, &length);
    return {};
}

// Splits a payload of `count` packed NUL-terminated strings into pointers.
static ErrorOr<Vector<GLchar const*>> split_packed_strings(ReadonlyBytes bytes, GLsizei count)
{
    if (count < 0 || count > max_webgl_string_list_entries)
        return Error::from_string_literal("WebGL string list is too long");
    Vector<GLchar const*> strings;
    strings.ensure_capacity(count);
    size_t cursor = 0;
    for (GLsizei i = 0; i < count; ++i) {
        auto start = cursor;
        while (cursor < bytes.size() && bytes[cursor] != 0)
            ++cursor;
        if (cursor >= bytes.size())
            return Error::from_string_literal("WebGL string is not NUL-terminated");
        strings.unchecked_append(reinterpret_cast<GLchar const*>(bytes.data() + start));
        ++cursor;
    }
    return strings;
}

ErrorOr<void> replay_webgl_command(OpenGLContext& gl, WebGLObjectMap& objects, Commands::TransformFeedbackVaryings const& command, ReadonlyBytes payload)
{
    auto varyings_bytes = WebGLCommandList::resolve_data_span(payload, command.varyings);
    auto varyings = TRY(split_packed_strings(varyings_bytes, command.count));
    auto program = objects.lookup(command.program);
    gl.transform_feedback_varyings(program, command.count, varyings.data(), command.buffer_mode);
    return {};
}

ErrorOr<ByteBuffer> handle_one(OpenGLContext& gl, WebGLObjectMap&, SyncCalls::GetString::Request const& request, ReadonlyBytes)
{
    auto const* value = gl.get_string(request.name);
    static constexpr u8 empty_string[] { 0 };
    auto value_bytes = value
        ? ReadonlyBytes { value, __builtin_strlen(reinterpret_cast<char const*>(value)) + 1 }
        : ReadonlyBytes { empty_string, sizeof(empty_string) };
    SyncCalls::GetString::Reply reply {
        .value = { WebGLCommandList::first_inline_data_offset(sizeof(SyncCalls::GetString::Reply)), static_cast<u32>(value_bytes.size()) },
    };
    return WebGLSyncCall::encode_reply(reply, value_bytes);
}

ErrorOr<ByteBuffer> handle_one(OpenGLContext& gl, WebGLObjectMap&, SyncCalls::GetVertexAttribPointervRobustANGLE::Request const& request, ReadonlyBytes)
{
    void* pointer = nullptr;
    GLsizei length = 0;
    gl.get_vertex_attrib_pointerv_robust_angle(request.index, request.pname, 1, &length, &pointer);
    SyncCalls::GetVertexAttribPointervRobustANGLE::Reply reply {
        .pointer = static_cast<Web::WebGL::GLintptr>(reinterpret_cast<uintptr_t>(pointer)),
    };
    return WebGLSyncCall::encode_reply(reply);
}

ErrorOr<ByteBuffer> handle_one(OpenGLContext& gl, WebGLObjectMap& objects, SyncCalls::GetUniformIndices::Request const& request, ReadonlyBytes payload)
{
    auto names_bytes = WebGLCommandList::resolve_data_span(payload, request.uniform_names);
    auto names = TRY(split_packed_strings(names_bytes, request.uniform_count));
    auto program = objects.lookup(request.program);
    Vector<GLuint> indices;
    indices.resize(request.uniform_count);
    gl.get_uniform_indices(program, request.uniform_count, names.data(), indices.data());
    ReadonlyBytes indices_bytes { indices.data(), indices.size() * sizeof(GLuint) };
    SyncCalls::GetUniformIndices::Reply reply {
        .uniform_indices = { WebGLCommandList::first_inline_data_offset(sizeof(SyncCalls::GetUniformIndices::Reply)), static_cast<u32>(indices_bytes.size()) },
    };
    return WebGLSyncCall::encode_reply(reply, indices_bytes);
}

}
