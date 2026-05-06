/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/Size.h>
#include <LibIPC/File.h>
#include <LibPaintServer/Types.h>

namespace WebView {

class BrokerOfPaintServer;

struct LinuxDmaBufPresentationBuffer {
    u32 drm_format { 0 };
    u32 stride { 0 };
    u32 offset { 0 };
    IPC::File fd;
    Gfx::IntSize size;
};

class Presentation {
public:
    virtual ~Presentation() = default;

    virtual void ensure_broker_owned_presentation_buffers(BrokerOfPaintServer&, PaintServer::SurfaceID surface_id, Gfx::IntSize size) = 0;

    virtual bool has_pool_image(PaintServer::SurfaceID surface_id, u64 image_id) const = 0;
    virtual Optional<void*> platform_surface_handle_for_image(PaintServer::SurfaceID surface_id, u64 image_id) = 0;

    virtual Optional<LinuxDmaBufPresentationBuffer> clone_linux_dmabuf_presentation_buffer(PaintServer::SurfaceID surface_id, u64 image_id) const = 0;
    virtual RefPtr<Gfx::Bitmap const> bitmap_for_presentation_image(PaintServer::SurfaceID surface_id, u64 image_id) const = 0;

    virtual void clear_surface(PaintServer::SurfaceID surface_id) = 0;
};

OwnPtr<Presentation> create_presentation_backend();

}
