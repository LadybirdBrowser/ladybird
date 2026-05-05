/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/Resource/Resource.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibPaintServer/Resource/SharedArena.h>
#include <LibPaintServer/ToPaintServerFromRenderClientEndpoint.h>
#include <LibPaintServer/ToRenderClientFromPaintServerEndpoint.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class RenderClientOfPaintServer final
    : public IPC::ConnectionToServer<ToRenderClientFromPaintServerEndpoint, ToPaintServerFromRenderClientEndpoint>
    , public ToRenderClientFromPaintServerEndpoint {
    C_OBJECT_ABSTRACT(RenderClientOfPaintServer);

public:
    explicit RenderClientOfPaintServer(NonnullOwnPtr<IPC::Transport>);
    virtual ~RenderClientOfPaintServer() override;

    static RenderClientOfPaintServer* get();

    struct HelloResponse {
        int peer_pid { -1 };
        GPUEpoch server_epoch { 0 };
        size_t max_texture_size { 0 };
        bool supports_arenas { false };
    };

    void hello(Function<void(HelloResponse)> callback = {}, bool is_headless = false);

    struct CreatedContentImage {
        bool success { false };
        ImageID image_id { 0 };
        Optional<Gfx::SharedImagePayload> content_image;
    };

    GPUEpoch server_epoch() const { return m_server_epoch; }
    void set_server_epoch(GPUEpoch server_epoch);

    ImageID allocate_image_id();
    ReleaseToken allocate_release_token() { return m_next_release_token++; }
    void create_content_image(ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, Function<void(CreatedContentImage)>);
    void import_content_image(ImageID image_id, Gfx::SharedImagePayload content_image, Function<void(bool)> callback);
    void complete_content_upload(ImageID image_id, bool success);
    void destroy_content_image(ImageID image_id, SyncToken wait_sync_token = 0);
    void arm_next_frame_logging();

    void submit_arena_packet(SurfaceID surface_id, SharedArena::Slice const& slice, SyncToken wait_sync_token, ReleaseToken release_token);

    Function<void()> on_death;
    Function<void(GPUEpoch server_epoch)> on_server_epoch_changed;
    Function<void(SurfaceID surface_id, ReleaseToken release_token, CompletionStatus status)> on_did_complete;
    Function<void(SurfaceID surface_id, RequestID request_id, ImageID image_id, Optional<Gfx::SharedImagePayload>, Optional<Gfx::ShareableBitmap>)> on_did_complete_offscreen_render;
    Function<void(SurfaceID surface_id)> on_request_frame_repaint;
    Function<void(SurfaceID surface_id, ResourceID resource_id, ReleaseToken release_token)> on_did_fail_resource_registration;

private:
    void flush_pending_create_content_images();
    void flush_pending_import_content_images();

    void die() override;
    void server_epoch_changed(GPUEpoch server_epoch) override;
    void did_hello(RequestID request_id, int peer_pid, GPUEpoch server_epoch, size_t max_texture_size) override;
    void did_complete(SurfaceID surface_id, ReleaseToken release_token, CompletionStatus status) override;
    void did_complete_offscreen_render(SurfaceID surface_id, RequestID request_id, ImageID image_id, Optional<Gfx::SharedImagePayload> content_image, Optional<Gfx::ShareableBitmap> bitmap) override;
    void request_frame_repaint(SurfaceID surface_id) override;
    void did_fail_resource_registration(SurfaceID surface_id, ResourceID resource_id, ReleaseToken release_token) override;

    void did_create_content_image(RequestID request_id, bool success, ImageID image_id, Optional<Gfx::SharedImagePayload> content_image) override;
    void did_import_content_image(RequestID request_id, bool success, ImageID image_id) override;

    GPUEpoch m_server_epoch { 0 };
    RequestID m_next_request_id { 1 };
    ImageID m_next_image_id { 1 };
    ReleaseToken m_next_release_token { 1 };

    struct PendingCreateContentImage {
        ImageID image_id { 0 };
        Gfx::IntSize size;
        Gfx::BitmapFormat format { Gfx::BitmapFormat::BGRA8888 };
        Function<void(CreatedContentImage)> callback;
    };

    struct PendingImportContentImage {
        ImageID image_id { 0 };
        Gfx::SharedImagePayload content_image;
        Function<void(bool)> callback;
    };

    Vector<PendingCreateContentImage> m_pending_create_content_images;
    Vector<PendingImportContentImage> m_pending_import_content_images;
    HashMap<RequestID, Function<void(HelloResponse)>> m_pending_hello;
    HashMap<RequestID, Function<void(CreatedContentImage)>> m_pending_create_content_image;
    HashMap<RequestID, Function<void(bool)>> m_pending_import_content_image;
};

}
