/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <Compositor/OpenGLContext.h>
#include <Compositor/WebGLObjectMap.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Compositing::WebGL::Commands {

struct TexImage2DFromBitmap;
struct TexSubImage2DFromBitmap;
struct TexImage3DFromBitmap;
struct TexSubImage3DFromBitmap;

}

namespace Compositor {

class HostWebGLContext {
public:
    AK_ALLOC_WITH_KMALLOC;

    static OwnPtr<HostWebGLContext> create(RefPtr<Gfx::SkiaBackendContext>, OpenGLContext::WebGLVersion, OpenGLContext::DrawingBufferOptions, Gfx::IntSize initial_size);

    ErrorOr<void> execute_commands(ReadonlyBytes, Vector<Gfx::DecodedImageFrame> const& bitmaps);

    void set_shared_command_buffer(Compositing::WebGL::WebGLSharedCommandBuffer shared_command_buffer) { m_shared_command_buffer = move(shared_command_buffer); }
    Optional<ReadonlyBytes> shared_command_buffer_range(u64 offset, u64 size_in_bytes) const;
    void store_executed_flush_sequence_number(u64 flush_sequence_number) { m_shared_command_buffer.store_executed_flush_sequence_number(flush_sequence_number); }
    ErrorOr<ByteBuffer> execute_sync_call(ReadonlyBytes request);
    Gfx::ShareableBitmap read_back_drawing_buffer(Gfx::IntRect);
    Compositing::WebGL::ReadPixelsResult read_pixels_robust_angle(Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels);
    bool read_buffer_sub_data(Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer data);
    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> prepare_for_compositing(bool preserve_drawing_buffer);
    RefPtr<Gfx::PaintingSurface> surface();

    OpenGLContext& gl_context() { return *m_gl_context; }

private:
    explicit HostWebGLContext(NonnullOwnPtr<OpenGLContext>);

    ErrorOr<void> set_drawing_buffer_size(int width, int height);
    ErrorOr<void> tex_image2d_from_bitmap(Compositing::WebGL::Commands::TexImage2DFromBitmap const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    ErrorOr<void> tex_sub_image2d_from_bitmap(Compositing::WebGL::Commands::TexSubImage2DFromBitmap const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    ErrorOr<void> tex_image3d_from_bitmap(Compositing::WebGL::Commands::TexImage3DFromBitmap const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    ErrorOr<void> tex_sub_image3d_from_bitmap(Compositing::WebGL::Commands::TexSubImage3DFromBitmap const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);

    NonnullOwnPtr<OpenGLContext> m_gl_context;
    WebGLObjectMap m_objects;
    Compositing::WebGL::WebGLSharedCommandBuffer m_shared_command_buffer;
    bool m_needs_clear_before_next_frame { false };
};

}
