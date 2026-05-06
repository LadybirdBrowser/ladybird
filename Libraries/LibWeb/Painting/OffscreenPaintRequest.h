/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibPaintServer/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>

namespace Web::Painting {

enum class OffscreenPaintSync : u8 {
    SubmitImmediately,
    WaitForExternalContent,
};

struct OffscreenPaintResult {
    PaintServer::ImageID image_id { 0 };
    Optional<Gfx::SharedImagePayload> content_image;
    Optional<Gfx::ShareableBitmap> bitmap;
};

struct OffscreenPaintRequest {
    GC::Ref<DOM::Document> document;
    HTML::PaintConfig paint_config;
    PaintServer::OffscreenRenderTarget target;
    OffscreenPaintSync readiness { OffscreenPaintSync::SubmitImmediately };
    Function<void(OffscreenPaintResult)> callback;
};

WEB_API bool submit_offscreen_paint_request(OffscreenPaintRequest, u64 painting_id, Function<void()> on_submit_error);

}
