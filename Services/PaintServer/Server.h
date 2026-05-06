/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Resource/Resource.h>
#include <LibGfx/Size.h>
#include <LibIPC/Transport.h>
#include <LibPaintServer/ToPaintServerFromBrokerEndpoint.h>
#include <LibPaintServer/Types.h>
#include <PaintServer/Painter.h>
#include <PaintServer/PresentationBufferState.h>
#include <PaintServer/RenderClient/Types.h>
#include <PaintServer/WorkerThread.h>

namespace PaintServer {

class BrokerConnection;
class Painter;
class RenderClientConnection;
class ResourceManager;

class Server {
public:
    explicit Server(NonnullOwnPtr<Painter>, u64 server_epoch);
    ~Server();

    // Creates and retains the broker connection. main() does not need to keep a local ref alive.
    void create_broker_connection(NonnullOwnPtr<IPC::Transport>);

    // Records the active broker connection. Server uses this to send frame_ready and resource events.
    void set_broker_connection(BrokerConnection&);

    // Drops the broker connection and shuts down the server. This also disconnects all render clients.
    void clear_broker_connection(BrokerConnection&);

    // Accepts a new render client connection. Called by the broker side when a web content process
    // is connected.
    bool connect_render_client(NonnullOwnPtr<IPC::Transport>);

    // Removes a render client and its GPU resources.
    void remove_render_client(RenderClientConnection&);
    bool has_render_client(ConnectionID) const;

    // Creates or updates the backing surface used for presentation.
    // If require_existing_surface is true, unknown surfaces are rejected.
    bool set_presentation_surface(SurfaceID surface_id, Gfx::IntSize size, bool require_existing_surface);

    // Controls whether the surface is considered visible. Used as a hint for resource use.
    void set_presentation_surface_visible(SurfaceID surface_id, bool visible);

    // Destroys the surface and any associated presentation buffer pool.
    void destroy_presentation_surface(SurfaceID surface_id);
    bool has_presentation_surface(SurfaceID surface_id) const;
    bool presentation_surface_dimensions_match(SurfaceID surface_id, Gfx::IntSize frame_size) const;

    enum class SubmitFrameResult : u8 {
        Submitted,
        Blocked,
        Failed,
    };

    SubmitFrameResult submit_frame_for_client_async(RenderClientConnection&, SurfaceID surface_id, NonnullRefPtr<IngressFrame>);
    void submit_canvas_draw_list_for_client_async(RenderClientConnection&, SurfaceID surface_id, ImageID, Gfx::IntSize, Gfx::BitmapFormat, ByteBuffer draw_list_payload, ReleaseToken);

    // Tells the broker a new frame is ready for a given present id.
    void notify_broker_frame_ready(SurfaceID surface_id, u64 present_id, ImageID image_id);

    // Feedback from the broker that a present id is no longer being used. This is what frees a
    // presentation buffer for reuse.
    void did_present_or_released(SurfaceID surface_id, u64 present_id);

    // Receives broker-owned presentation buffers and imports them into the GPU backend.
    void register_presentation_buffers(SurfaceID surface_id, Vector<ImageID> image_ids, Vector<Gfx::SharedImagePayload> image_payloads, Gfx::IntSize buffer_size);

    void register_resource(ConnectionID connection_id, SurfaceID surface_id, Gfx::ResourceInfo descriptor, ByteBuffer resource_data, Function<void(bool success)> callback = {});
    void unregister_resource(ConnectionID connection_id, SurfaceID surface_id, ResourceID resource_id, Function<void()> callback = {});

    void create_content_image_for_client(ConnectionID connection_id, RequestID request_id, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format);
    void import_content_image_for_client(ConnectionID connection_id, RequestID request_id, ImageID image_id, Gfx::SharedImagePayload content_image);
    void complete_content_upload_for_client(ConnectionID connection_id, ImageID image_id, bool success);
    void destroy_content_image_for_client(ConnectionID connection_id, ImageID image_id);

    u64 server_epoch() const { return m_server_epoch; }

private:
    friend class RenderClientConnection;
    friend class ResourceManager;

    struct AsyncSubmitRequest {
        RefPtr<RenderClientConnection> connection;
        NonnullRefPtr<IngressFrame> frame;
        ConnectionID connection_id { 0 };
        SurfaceID surface_id { 0 };
        ReleaseToken release_token { 0 };
        Optional<u64> reserved_presentation_buffer_id;
        Gfx::IntSize presentation_frame_size;
    };

    struct SurfaceConfiguration {
        Gfx::IntSize logical_size;
        Gfx::IntSize buffer_size;
    };

    void clear_presentation_buffers_for_surface(SurfaceID surface_id);
    void did_complete_surface_configuration(SurfaceID surface_id, u64 generation, bool success);
    void post_to_main_thread(Function<void()> callback);
    void post_submit_result_to_main_thread(AsyncSubmitRequest request, Painter::SubmitResult result);
    bool post_to_compositor_thread(Function<void()> callback);
    void finish_submit_on_compositor_thread(AsyncSubmitRequest const& request);
    bool render_canvas_draw_list_on_compositor_thread(RenderClientConnection&, SurfaceID surface_id, ImageID, Gfx::IntSize, Gfx::BitmapFormat, ReadonlyBytes draw_list_payload);

    struct SurfaceRecord {
        struct PendingConfiguration {
            SurfaceConfiguration configuration;
            u64 generation { 0 };
        };

        SurfaceConfiguration configuration;
        Optional<PendingConfiguration> pending_configuration;
        u64 last_configuration_generation { 0 };
        u64 presentation_buffer_generation { 0 };
        bool visible { true };
    };

    void did_import_presentation_buffer(SurfaceID surface_id, ImageID image_id, u64 generation);
    void import_or_queue_presentation_buffer(SurfaceID surface_id, ImageID image_id, Gfx::SharedImagePayload&& image_payload, u64 generation);

    NonnullOwnPtr<Painter> m_surface_backend;
    RefPtr<BrokerConnection> m_broker_connection;
    HashMap<ConnectionID, RefPtr<RenderClientConnection>> m_render_client_connections;
    HashMap<SurfaceID, SurfaceRecord> m_surfaces;
    u64 m_server_epoch { 1 };
    RefPtr<Core::WeakEventLoopReference> m_main_event_loop;
    WorkerThread m_compositor_worker;
    PresentationBufferState m_presentation_buffers;
};

}
