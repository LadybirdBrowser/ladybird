/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>

namespace Web::Compositor {

class WEB_API CompositorMainThreadClient : public AtomicRefCounted<CompositorMainThreadClient> {
public:
    virtual ~CompositorMainThreadClient() = default;

    virtual void request_rendering_update() = 0;
    virtual void did_complete_screenshot(ScreenshotRequestId) = 0;
    virtual void did_fail_screenshot(ScreenshotRequestId) = 0;
    virtual void did_lose_compositor() = 0;
};

class WEB_API CompositorUIPresentationClient : public AtomicRefCounted<CompositorUIPresentationClient> {
public:
    virtual ~CompositorUIPresentationClient() = default;

    virtual void publish_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::SharedImage&& front_backing_store, i32 back_bitmap_id, Gfx::SharedImage&& back_backing_store) = 0;
    virtual void present_frame_to_ui(u64 page_id, Gfx::IntRect const& content_rect, i32 bitmap_id) = 0;
};

}
