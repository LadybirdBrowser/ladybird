/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>

namespace Web::HTML {

class WEB_API RemoteCanvas2DTransport : public RefCounted<RemoteCanvas2DTransport> {
public:
    virtual ~RemoteCanvas2DTransport() = default;

    virtual Optional<Painting::CanvasId> create_context(Gfx::IntSize, bool alpha) = 0;
    virtual void destroy_context(Painting::CanvasId) = 0;
    virtual void update_commands(Painting::CanvasId, Gfx::CanvasCommandList const&) = 0;

    virtual RefPtr<Gfx::Bitmap> read_back_pixels(Painting::CanvasId, Gfx::IntRect const&) = 0;
};

}
