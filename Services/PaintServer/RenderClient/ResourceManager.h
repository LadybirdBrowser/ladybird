/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibIPC/File.h>
#include <PaintServer/RenderClient/Types.h>
#include <core/SkRefCnt.h>

class SkImage;

namespace Gfx {

class PaintingSurface;
class SharedImage;
struct ResourceInfo;

}

namespace PaintServer {

class Painter;
class RenderClientConnection;

class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();

    void reset();

    bool register_arena(ArenaID arena_id, IPC::File arena_handle, size_t arena_size);
    void unregister_arena(ArenaID arena_id);
    Optional<ReadonlyBytes> arena_slice(ArenaID arena_id, size_t offset, size_t size) const;

    ErrorOr<void> register_bitmap_resource(ConnectionID, Gfx::ResourceInfo const&, Gfx::SharedImagePayload image_payload);
    void unregister_bitmap_resource(ResourceID resource_id);
    ErrorOr<Gfx::SharedImagePayload> allocate_content_image(ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, Painter&, bool is_headless);
    ErrorOr<void> import_content_image(ImageID image_id, Gfx::SharedImagePayload);
    void complete_image_upload(ImageID image_id, bool success);
    void destroy_image(ImageID image_id);
    void destroy_all_images();
    sk_sp<SkImage> resolve_image(ResourceID resource_id, ImageID image_id, Painter&, bool is_headless);
    Gfx::SharedImage* shared_image(ImageID image_id);
    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> canvas_painting_surface(ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, Painter&, bool is_headless);

    void register_resource_from_shared_blob(RenderClientConnection&, SurfaceID, Gfx::ResourceInfo, IPC::File blob_handle, size_t blob_size, ReleaseToken release_token);

private:
    class Impl;
    static ErrorOr<void> store_image(Impl&, ImageID image_id, Gfx::SharedImage shared_image, bool is_uploaded);
    static ErrorOr<Gfx::SharedImagePayload> allocate_headless_content_image(Impl&, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format);

    OwnPtr<Impl> m_impl;
};

}
