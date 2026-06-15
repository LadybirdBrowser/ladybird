/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WEB_API RemoteWebGLTransport : public RefCounted<RemoteWebGLTransport> {
public:
    virtual ~RemoteWebGLTransport() = default;

    struct CreateResult {
        bool success { false };
        Vector<String> supported_extensions;
    };
    virtual CreateResult create_context(WebGLVersion, Gfx::IntSize initial_size, bool depth, bool stencil, bool antialias) = 0;
    virtual Optional<Painting::CanvasId> canvas_id() const = 0;
    virtual void destroy_context() = 0;

    virtual void send_commands(ByteBuffer const&, Vector<Gfx::DecodedImageFrame> const& bitmaps) = 0;
    virtual void present_canvas(bool preserve_drawing_buffer) = 0;
    virtual ByteBuffer sync_call(ByteBuffer request) = 0;
    virtual ReadPixelsResult read_pixels_robust_angle(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, Core::AnonymousBuffer pixels) = 0;
    virtual void read_buffer_sub_data(GLenum target, GLintptr offset, GLintptr size, Core::AnonymousBuffer data) = 0;

    virtual Gfx::ShareableBitmap read_back_drawing_buffer(Gfx::IntRect const&) = 0;
};

}
