/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Resource/Resource.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibPaintServer/ToPaintServerFromRenderClientEndpoint.h>
#include <LibPaintServer/ToRenderClientFromPaintServerEndpoint.h>
#include <PaintServer/FontResourceCache.h>
#include <PaintServer/RenderClient/CompletionTracker.h>
#include <PaintServer/RenderClient/ResourceManager.h>
#include <PaintServer/RenderClient/Types.h>

namespace PaintServer {

class Server;

class RenderClientConnection final
    : public IPC::ConnectionFromClient<ToRenderClientFromPaintServerEndpoint, ToPaintServerFromRenderClientEndpoint> {
    C_OBJECT(RenderClientConnection);

    friend class Server;
    friend class ResourceManager;

public:
    ~RenderClientConnection() override = default;

    ConnectionID connection_id() const { return static_cast<ConnectionID>(client_id()); }

    // Closes this IPC connection and releases resources. Called by the IPC layer when the client dies
    // or when we detect a fatal protocol problem.
    void die() override;

    pid_t client_pid() const { return m_client_pid; }
    bool is_headless() const { return m_is_headless; }

    ResourceManager& resource_manager() { return m_resource_manager; }

    HashTable<SurfaceID> const& referenced_surface_ids() const { return m_referenced_surface_ids; }
    void did_create_content_image(RequestID request_id, bool success, ImageID image_id, Optional<Gfx::SharedImagePayload> content_image);
    void did_import_content_image(RequestID request_id, bool success, ImageID image_id);
    bool wait_sync_token_has_been_ingested(SyncToken wait_sync_token) const;
    bool wait_sync_token_has_been_rendered(SyncToken wait_sync_token) const;
    void try_dispatch_frames();
    void presentation_buffer_available(SurfaceID surface_id);
    void did_complete_render(SurfaceID surface_id, ReleaseToken release_token, bool rendered);
    void clear_submit_state_for_surface(SurfaceID surface_id);
    void clear_compositor_resources_for_surface(SurfaceID surface_id);
    void clear_compositor_resources();

private:
    RenderClientConnection(Server&, NonnullOwnPtr<IPC::Transport>);

    // Runs deferred shared image destroys that were waiting for earlier work to finish.
    void flush_pending_image_destroys();
    bool are_sync_tokens_valid(SyncToken wait_sync_token, ReleaseToken release_token) const;
    void enqueue_release_token(SurfaceID surface_id, ReleaseToken release_token);
    void maybe_request_frame_repaint_after_resource_resolution(SurfaceID surface_id);
    void complete_resource_registration_at_ingress(SurfaceID surface_id, ResourceID resource_id, ReleaseToken release_token, bool success);
    void reject_submit_at_ingress(SurfaceID surface_id, ReleaseToken release_token);
    bool stage_frame_at_ingress(SurfaceID surface_id, FrameHeader const& header, ReadonlyBytes payload, SyncToken wait_sync_token, SyncToken render_wait_sync_token, ReleaseToken release_token);
    bool try_dispatch_surface_frame(SurfaceID surface_id);
    void fail_offscreen_render(SurfaceID surface_id, FrameHeader const& header);
    bool handle_frame_packet_at_ingress(SurfaceID surface_id, ReadonlyBytes packet_bytes, SyncToken wait_sync_token, ReleaseToken release_token);
    void handle_canvas_packet_at_ingress(SurfaceID surface_id, ReadonlyBytes packet_bytes, SyncToken wait_sync_token, ReleaseToken release_token);
    void handle_resource_packet_at_ingress(SurfaceID surface_id, ReadonlyBytes packet_bytes, ReleaseToken release_token);

    struct SurfaceFrameSlots {
        RefPtr<IngressFrame> presentation_frame;
        Vector<NonnullRefPtr<IngressFrame>> offscreen_frames;
        RefPtr<IngressFrame> render_frame;
        bool repaint_request_pending { false };
    };

    void fail_pending_frames_for_surface(SurfaceID surface_id, SurfaceFrameSlots&);

    enum class FrameSubmitResult : u8 {
        Submitted,
        Blocked,
        Failed,
    };

    FrameSubmitResult submit_frame(SurfaceID surface_id, SurfaceFrameSlots&, NonnullRefPtr<IngressFrame>);

    struct PendingImageDestroy {
        ImageID image_id { 0 };
        SyncToken wait_sync_token { 0 };
    };

    // Handshake from a render client. Establishes identity and reports server capabilities.
    void hello(RequestID request_id, ClientKind client_kind, int client_pid, bool is_headless) override;
    void register_arena(ArenaID arena_id, IPC::File arena_handle, size_t arena_size) override { resource_manager().register_arena(arena_id, move(arena_handle), arena_size); }
    void unregister_arena(ArenaID arena_id) override { resource_manager().unregister_arena(arena_id); }
    // Main render entrypoint. Validates the payload and stages the latest frame for this surface.
    void submit_arena_packet(SurfaceID surface_id, ArenaID arena_id, size_t offset, size_t size, SyncToken wait_sync_token, ReleaseToken release_token) override;
    void register_resource_from_shared_blob(SurfaceID surface_id, Gfx::ResourceInfo descriptor, IPC::File blob_handle, size_t blob_size, ReleaseToken release_token) override;
    void register_bitmap_resource_from_shared_image(SurfaceID surface_id, Gfx::ResourceInfo descriptor, Gfx::SharedImagePayload image_payload, ReleaseToken release_token) override;
    void unregister_resource(SurfaceID surface_id, ResourceID resource_id) override;

    // Allocates mutable content backing and sends a transferable handle back to the client.
    void create_content_image(RequestID request_id, GPUEpoch server_epoch, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format) override;
    void import_content_image(RequestID request_id, GPUEpoch server_epoch, ImageID image_id, Gfx::SharedImagePayload content_image) override;
    void complete_content_upload(GPUEpoch server_epoch, ImageID image_id, bool success) override;
    // Releases mutable content backing. This may be delayed until the render-completion stream reaches wait_sync_token.
    void destroy_content_image(GPUEpoch server_epoch, ImageID image_id, SyncToken wait_sync_token) override;
    void arm_next_frame_logging() override;

    ResourceManager m_resource_manager;
    FontResourceCache m_compositor_font_cache;
    CompletionTracker m_ingested_completions;
    CompletionTracker m_render_completions;

    Vector<PendingImageDestroy> m_pending_image_destroys;

    HashTable<SurfaceID> m_referenced_surface_ids;
    HashMap<SurfaceID, SurfaceFrameSlots> m_surface_frames;
    ReleaseToken m_last_submitted_release_token { 0 };

    Server& m_server;
    pid_t m_client_pid { -1 };
    bool m_is_headless { false };
};

}
