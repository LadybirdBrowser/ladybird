/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <Compositor/BitmapExport.h>
#include <Compositor/HostWebGLContext.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VideoFrameSkia.h>
#include <Compositor/WebGLCommandReplayer.h>
#include <LibCompositing/WebGL/TextureUpload.h>
#include <LibCompositing/WebGL/WebGLCommandList.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibMediaCodecs/VideoFrame.h>

#include <core/SkImage.h>

namespace Compositor {

using namespace Compositing::WebGL;

static constexpr GLsizei max_webgl_string_list_entries = 16384;

HostWebGLContext::HostWebGLContext(NonnullOwnPtr<OpenGLContext> gl_context, RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
    : m_gl_context(move(gl_context))
    , m_skia_backend_context(move(skia_backend_context))
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
    return adopt_own(*new HostWebGLContext(gl_context.release_nonnull(), move(skia_backend_context)));
}

Optional<ReadonlyBytes> HostWebGLContext::shared_command_buffer_range(u64 offset, u64 size_in_bytes) const
{
    if (!m_shared_command_buffer.is_valid())
        return {};
    auto data_region = m_shared_command_buffer.data_region();
    if (offset % WebGLCommandList::command_alignment != 0)
        return {};
    if (offset > data_region.size() || size_in_bytes > data_region.size() - offset)
        return {};
    return data_region.slice(offset, size_in_bytes);
}

ErrorOr<void> HostWebGLContext::execute_commands(ReadonlyBytes bytes, Vector<UploadSource> const& sources)
{
    m_gl_context->make_current();
    clear_drawing_buffer_if_needed();

    return WebGLCommandList::for_each_command(bytes, [&]<typename Command>(Command const& command, [[maybe_unused]] ReadonlyBytes payload) -> ErrorOr<void> {
        if constexpr (IsSame<Command, Commands::SetDrawingBufferSize>) {
            return set_drawing_buffer_size(command.width, command.height);
        } else if constexpr (IsSame<Command, Commands::ReadPixelsIntoPixelPackBuffer>) {
            m_gl_context->read_pixels_robust_angle(command.x, command.y, command.width, command.height, command.format, command.type, 0, nullptr, nullptr, nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(command.offset)));
            return {};
        } else if constexpr (IsSame<Command, Commands::TexImage2DFromBitmap>) {
            return tex_image2d_from_bitmap(command, sources);
        } else if constexpr (IsSame<Command, Commands::TexSubImage2DFromBitmap>) {
            return tex_sub_image2d_from_bitmap(command, sources);
        } else if constexpr (IsSame<Command, Commands::TexImage3DFromBitmap>) {
            return tex_image3d_from_bitmap(command, sources);
        } else if constexpr (IsSame<Command, Commands::TexSubImage3DFromBitmap>) {
            return tex_sub_image3d_from_bitmap(command, sources);
        } else {
            return replay_webgl_command(*m_gl_context, m_objects, command, payload);
        }
    });
}

static ErrorOr<HostWebGLContext::UploadSource const*> upload_source_at(Vector<HostWebGLContext::UploadSource> const& sources, u32 index)
{
    if (index >= sources.size())
        return Error::from_string_literal("WebGL image upload references an out-of-range bitmap");
    return &sources[index];
}

static ErrorOr<Gfx::DecodedImageFrame> decoded_frame_for_upload(HostWebGLContext::UploadSource const& source, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    return source.visit(
        [](Gfx::DecodedImageFrame const& frame) -> ErrorOr<Gfx::DecodedImageFrame> {
            return frame;
        },
        [&](NonnullRefPtr<Media::VideoFrame> const& video_frame) -> ErrorOr<Gfx::DecodedImageFrame> {
            return decoded_image_frame_from_video_frame(*video_frame, skia_backend_context);
        });
}

static ErrorOr<Gfx::BitmapExportResult> convert_bitmap_for_upload(Vector<HostWebGLContext::UploadSource> const& sources, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, u32 bitmap_index, GLenum format, GLenum type, bool has_explicit_destination_size, GLsizei destination_width, GLsizei destination_height, bool flip_y, bool premultiply_alpha)
{
    auto const& source = *TRY(upload_source_at(sources, bitmap_index));

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

    auto frame = TRY(decoded_frame_for_upload(source, skia_backend_context));
    return Gfx::export_bitmap_to_byte_buffer(frame.bitmap(), frame.color_space(), export_format.value(), export_flags, target_width, target_height);
}

static bool copy_can_write_internal_format(GLint internalformat)
{
    switch (internalformat) {
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA:
    case GL_RGB:
    case GL_RGBA:
    case GL_SRGB_EXT:
    case GL_SRGB_ALPHA_EXT:
    case GL_R8:
    case GL_RG8:
    case GL_RGB8:
    case GL_RGBA8:
    case GL_SRGB8:
    case GL_SRGB8_ALPHA8:
    case GL_RGB565:
    case GL_RGBA4:
    case GL_RGB5_A1:
    case GL_RGB10_A2:
    case GL_RGB9_E5:
    case GL_R11F_G11F_B10F:
    case GL_R16F:
    case GL_RG16F:
    case GL_RGB16F:
    case GL_RGBA16F:
    case GL_R32F:
    case GL_RG32F:
    case GL_RGB32F:
    case GL_RGBA32F:
        return true;
    default:
        return false;
    }
}

static bool format_is_integer(GLenum format)
{
    return format == GL_RED_INTEGER || format == GL_RG_INTEGER || format == GL_RGB_INTEGER || format == GL_RGBA_INTEGER;
}

static Optional<Gfx::IntSize> upload_size(Media::VideoFrame const& frame, bool has_explicit_destination_size, GLsizei destination_width, GLsizei destination_height)
{
    if (!has_explicit_destination_size)
        return Gfx::IntSize { static_cast<int>(frame.width()), static_cast<int>(frame.height()) };
    if (destination_width < 0 || destination_height < 0)
        return {};
    return Gfx::IntSize { destination_width, destination_height };
}

class PixelUnpackBufferUnbinder {
public:
    explicit PixelUnpackBufferUnbinder(OpenGLContext& gl_context)
        : m_gl_context(gl_context)
    {
        if (m_gl_context.webgl_version() != WebGLVersion::WebGL2)
            return;
        m_gl_context.get_integerv_robust_angle(GL_PIXEL_UNPACK_BUFFER_BINDING, 1, nullptr, &m_bound_buffer);
        m_gl_context.bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    ~PixelUnpackBufferUnbinder()
    {
        if (m_gl_context.webgl_version() == WebGLVersion::WebGL2)
            m_gl_context.bind_buffer(GL_PIXEL_UNPACK_BUFFER, m_bound_buffer);
    }

private:
    OpenGLContext& m_gl_context;
    GLint m_bound_buffer { 0 };
};

// The level is defined by the same call that an upload of pixels would make, so that WebGL sees the same errors.
bool HostWebGLContext::define_texture_image_from_video_frame(Commands::TexImage2DFromBitmap const& command, Media::VideoFrame const& frame)
{
    if (!m_gl_context->can_copy_images_into_textures() || !copy_can_write_internal_format(command.internalformat))
        return false;
    auto size = upload_size(frame, command.has_explicit_destination_size, command.destination_width, command.destination_height);
    if (!size.has_value())
        return false;
    auto image = sk_image_from_video_frame(frame, m_skia_backend_context);
    if (!image)
        return false;

    m_gl_context->stash_errors();
    {
        PixelUnpackBufferUnbinder unbinder { *m_gl_context };
        m_gl_context->tex_image2d_robust_angle(command.target, command.level, command.internalformat, size->width(), size->height(), 0, command.format, command.type, 0, nullptr);
    }
    if (m_gl_context->stash_errors() || size->is_empty())
        return true;
    if (!m_gl_context->copy_image_into_bound_texture(*image, frame.size().to_type<int>(), *size, command.target, command.level, 0, 0, command.flip_y, command.premultiply_alpha))
        return false;
    m_gl_context->discard_errors();
    return true;
}

bool HostWebGLContext::update_texture_image_from_video_frame(Commands::TexSubImage2DFromBitmap const& command, Media::VideoFrame const& frame)
{
    if (!m_gl_context->can_copy_images_into_textures() || format_is_integer(command.format))
        return false;
    auto size = upload_size(frame, command.has_explicit_destination_size, command.destination_width, command.destination_height);
    if (!size.has_value())
        return false;
    auto image = sk_image_from_video_frame(frame, m_skia_backend_context);
    if (!image)
        return false;

    m_gl_context->stash_errors();
    {
        PixelUnpackBufferUnbinder unbinder { *m_gl_context };
        m_gl_context->tex_sub_image2d_robust_angle(command.target, command.level, command.xoffset, command.yoffset, 0, 0, command.format, command.type, 0, nullptr);
    }
    if (m_gl_context->stash_errors() || size->is_empty())
        return true;
    if (!m_gl_context->copy_image_into_bound_texture(*image, frame.size().to_type<int>(), *size, command.target, command.level, command.xoffset, command.yoffset, command.flip_y, command.premultiply_alpha))
        return false;
    // A region beyond the level is the caller's error, as it would be for an upload of pixels.
    m_gl_context->stash_errors();
    return true;
}

ErrorOr<void> HostWebGLContext::tex_image2d_from_bitmap(Commands::TexImage2DFromBitmap const& command, Vector<UploadSource> const& sources)
{
    if (auto const* video_frame = TRY(upload_source_at(sources, command.bitmap_index))->get_pointer<NonnullRefPtr<Media::VideoFrame>>(); video_frame && define_texture_image_from_video_frame(command, **video_frame))
        return {};
    auto converted = TRY(convert_bitmap_for_upload(sources, m_skia_backend_context, command.bitmap_index, command.format, command.type, command.has_explicit_destination_size, command.destination_width, command.destination_height, command.flip_y, command.premultiply_alpha));
    m_gl_context->tex_image2d_robust_angle(command.target, command.level, command.internalformat, converted.width, converted.height, 0, command.format, command.type, converted.buffer.size(), converted.buffer.data());
    return {};
}

ErrorOr<void> HostWebGLContext::tex_sub_image2d_from_bitmap(Commands::TexSubImage2DFromBitmap const& command, Vector<UploadSource> const& sources)
{
    if (auto const* video_frame = TRY(upload_source_at(sources, command.bitmap_index))->get_pointer<NonnullRefPtr<Media::VideoFrame>>(); video_frame && update_texture_image_from_video_frame(command, **video_frame))
        return {};
    auto converted = TRY(convert_bitmap_for_upload(sources, m_skia_backend_context, command.bitmap_index, command.format, command.type, command.has_explicit_destination_size, command.destination_width, command.destination_height, command.flip_y, command.premultiply_alpha));
    m_gl_context->tex_sub_image2d_robust_angle(command.target, command.level, command.xoffset, command.yoffset, converted.width, converted.height, command.format, command.type, converted.buffer.size(), converted.buffer.data());
    return {};
}

ErrorOr<void> HostWebGLContext::tex_image3d_from_bitmap(Commands::TexImage3DFromBitmap const& command, Vector<UploadSource> const& sources)
{
    auto converted = TRY(convert_bitmap_for_upload(sources, m_skia_backend_context, command.bitmap_index, command.format, command.type, command.has_explicit_destination_size, command.destination_width, command.destination_height, command.flip_y, command.premultiply_alpha));
    m_gl_context->tex_image3d_robust_angle(command.target, command.level, command.internalformat, converted.width, converted.height, command.depth, 0, command.format, command.type, converted.buffer.size(), converted.buffer.data());
    return {};
}

ErrorOr<void> HostWebGLContext::tex_sub_image3d_from_bitmap(Commands::TexSubImage3DFromBitmap const& command, Vector<UploadSource> const& sources)
{
    auto converted = TRY(convert_bitmap_for_upload(sources, m_skia_backend_context, command.bitmap_index, command.format, command.type, command.has_explicit_destination_size, command.destination_width, command.destination_height, command.flip_y, command.premultiply_alpha));
    m_gl_context->tex_sub_image3d_robust_angle(command.target, command.level, command.xoffset, command.yoffset, command.zoffset, converted.width, converted.height, command.depth, command.format, command.type, converted.buffer.size(), converted.buffer.data());
    return {};
}

// A non-preserving context's drawing buffer is cleared after being prepared for
// compositing, but the clear is deferred until the next GL command or read. Until
// then the presented frame is still the canvas content, so readbacks take it.
void HostWebGLContext::clear_drawing_buffer_if_needed()
{
    if (!m_needs_clear_before_next_frame)
        return;
    m_gl_context->clear_buffer_to_default_values();
    m_needs_clear_before_next_frame = false;
}

ErrorOr<ByteBuffer> HostWebGLContext::execute_sync_call(ReadonlyBytes request)
{
    m_gl_context->make_current();
    clear_drawing_buffer_if_needed();
    return handle_webgl_sync_call(*m_gl_context, m_objects, request);
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> HostWebGLContext::prepare_for_compositing(bool preserve_drawing_buffer)
{
    // Nothing ran since the last present, so that frame is still the canvas content. The drawing buffer holds
    // whatever an earlier frame left in it until the deferred clear.
    if (m_needs_clear_before_next_frame) {
        if (auto presented_surface = m_gl_context->presented_surface())
            return presented_surface.release_nonnull();
    }

    m_gl_context->present_drawing_buffer(preserve_drawing_buffer);

    auto presented_surface = m_gl_context->presented_surface();
    if (!presented_surface)
        return Error::from_string_literal("WebGL context has no drawing buffer");

    if (!preserve_drawing_buffer)
        m_needs_clear_before_next_frame = true;

    return presented_surface.release_nonnull();
}

void HostWebGLContext::clear_drawing_buffer()
{
    m_gl_context->make_current();
    m_gl_context->clear_buffer_to_default_values();
    m_needs_clear_before_next_frame = false;
}

RefPtr<Gfx::PaintingSurface> HostWebGLContext::presented_surface()
{
    return m_gl_context->presented_surface();
}

Gfx::ShareableBitmap HostWebGLContext::read_back_drawing_buffer(Gfx::IntRect rect)
{
    m_gl_context->make_current();
    m_gl_context->flush_drawing_buffer();
    auto surface = m_needs_clear_before_next_frame ? m_gl_context->presented_surface() : m_gl_context->drawing_buffer_surface();
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
    clear_drawing_buffer_if_needed();

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

bool HostWebGLContext::read_buffer_sub_data(GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer data)
{
    VERIFY(size >= 0);
    VERIFY(static_cast<size_t>(size) <= data.size());

    m_gl_context->make_current();

    if (auto* mapped = m_gl_context->map_buffer_range(target, offset, size, GL_MAP_READ_BIT)) {
        __builtin_memcpy(data.data<void>(), mapped, static_cast<size_t>(size));
        m_gl_context->unmap_buffer(target);
        return true;
    }
    return false;
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
        .pointer = static_cast<Compositing::WebGL::GLintptr>(reinterpret_cast<uintptr_t>(pointer)),
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
