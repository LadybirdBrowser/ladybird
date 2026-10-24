/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// What a 2D canvas records through: the context in the Compositor, and the stream of commands that WebContent shares
// between all of its canvases. Records are records of the display list tape, which name the fonts and the image
// frames that the stream carries beside them by id.
class WEB_API RemoteCanvas2DTransport : public RefCounted<RemoteCanvas2DTransport> {
public:
    virtual ~RemoteCanvas2DTransport() = default;

    virtual bool create_context(Gfx::IntSize, bool alpha, bool will_read_frequently) = 0;
    virtual Optional<Compositing::CanvasId> canvas_id() const = 0;
    virtual void destroy_context() = 0;

    // Appends one record for the canvas of the context to the shared stream.
    virtual void append_command(ReadonlyBytes record) = 0;
    virtual Compositing::FontResourceId add_font(Gfx::Font const&) = 0;
    virtual Compositing::ImageFrameResourceId add_image_frame(Gfx::DecodedImageFrame const&) = 0;
    // Keeps a video frame that a record draws until the Compositor has drawn it.
    virtual void retain_video_frame_hold(NonnullRefPtr<Compositing::VideoFrameHold>) = 0;
    // Marks the canvas as presentable at this point of the shared stream.
    virtual void record_present() = 0;
    // The number of records that the shared stream holds for all canvases.
    virtual size_t pending_command_count() const = 0;
    virtual void flush() = 0;

    // Unpremultiplied pixels come back as RGBA8888 and premultiplied pixels as BGRA8888.
    virtual RefPtr<Gfx::Bitmap> read_back_pixels(Gfx::IntRect const&, Gfx::AlphaType) = 0;
};

}
