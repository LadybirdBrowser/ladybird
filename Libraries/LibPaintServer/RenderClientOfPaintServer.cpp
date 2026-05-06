/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPaintServer/RenderClientOfPaintServer.h>

#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibPaintServer/Debug.h>

namespace PaintServer {

static RenderClientOfPaintServer* s_current_render_client = nullptr;

RenderClientOfPaintServer::RenderClientOfPaintServer(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToRenderClientFromPaintServerEndpoint, ToPaintServerFromRenderClientEndpoint>(*this, move(transport))
{
    s_current_render_client = this;
}

RenderClientOfPaintServer::~RenderClientOfPaintServer()
{
    if (s_current_render_client == this)
        s_current_render_client = nullptr;
}

RenderClientOfPaintServer* RenderClientOfPaintServer::get()
{
    return s_current_render_client;
}

void RenderClientOfPaintServer::hello(Function<void(HelloResponse)> callback, bool is_headless)
{
    RequestID request_id = m_next_request_id++;
    if (callback)
        m_pending_hello.set(request_id, move(callback));

    async_hello(request_id, ClientKind::RenderClient, Core::System::getpid(), is_headless);
}

ImageID RenderClientOfPaintServer::allocate_image_id()
{
    return ++m_next_image_id;
}

void RenderClientOfPaintServer::set_server_epoch(GPUEpoch server_epoch)
{
    if (m_server_epoch == server_epoch)
        return;

    m_server_epoch = server_epoch;
    flush_pending_create_content_images();
    flush_pending_import_content_images();
}

void RenderClientOfPaintServer::flush_pending_create_content_images()
{
    if (m_server_epoch == 0 || m_pending_create_content_images.is_empty())
        return;

    Vector<PendingCreateContentImage> pending_requests;
    pending_requests = move(m_pending_create_content_images);

    for (auto& request : pending_requests)
        create_content_image(request.image_id, request.size, request.format, move(request.callback));
}

void RenderClientOfPaintServer::flush_pending_import_content_images()
{
    if (m_server_epoch == 0 || m_pending_import_content_images.is_empty())
        return;

    Vector<PendingImportContentImage> pending_requests;
    pending_requests = move(m_pending_import_content_images);

    for (auto& request : pending_requests)
        import_content_image(request.image_id, move(request.content_image), move(request.callback));
}

void RenderClientOfPaintServer::create_content_image(ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, Function<void(CreatedContentImage)> callback)
{
    if (!callback)
        return;

    if (m_server_epoch == 0) {
        m_pending_create_content_images.append(PendingCreateContentImage {
            .image_id = image_id,
            .size = size,
            .format = format,
            .callback = move(callback),
        });
        return;
    }

    RequestID request_id = m_next_request_id++;

    m_pending_create_content_image.set(request_id, move(callback));
    async_create_content_image(request_id, m_server_epoch, image_id, size, format);
}

void RenderClientOfPaintServer::import_content_image(ImageID image_id, Gfx::SharedImagePayload content_image, Function<void(bool)> callback)
{
    if (!callback)
        return;

    if (m_server_epoch == 0) {
        m_pending_import_content_images.append(PendingImportContentImage {
            .image_id = image_id,
            .content_image = move(content_image),
            .callback = move(callback),
        });
        return;
    }

    RequestID request_id = m_next_request_id++;

    m_pending_import_content_image.set(request_id, move(callback));
    async_import_content_image(request_id, m_server_epoch, image_id, move(content_image));
}

void RenderClientOfPaintServer::complete_content_upload(ImageID image_id, bool success)
{
    async_complete_content_upload(m_server_epoch, image_id, success);
}

void RenderClientOfPaintServer::destroy_content_image(ImageID image_id, SyncToken wait_sync_token)
{
    async_destroy_content_image(m_server_epoch, image_id, wait_sync_token);
}

void RenderClientOfPaintServer::arm_next_frame_logging()
{
    async_arm_next_frame_logging();
}

void RenderClientOfPaintServer::submit_arena_packet(SurfaceID surface_id, SharedArena::Slice const& slice, SyncToken wait_sync_token, ReleaseToken release_token)
{
    if (!slice.is_valid()) {
        dbgln("RenderClientOfPaintServer: invalid slice for surface_id={}, arena_id={}, offset={}, size={}", surface_id, slice.arena_id, slice.offset, slice.size);
        return;
    }
    async_submit_arena_packet(surface_id, slice.arena_id, slice.offset, slice.size, wait_sync_token, release_token);
}

void RenderClientOfPaintServer::die()
{
    auto on_death_callback = move(on_death);
    if (on_death_callback)
        on_death_callback();
}

void RenderClientOfPaintServer::server_epoch_changed(GPUEpoch server_epoch)
{
    set_server_epoch(server_epoch);
    if (on_server_epoch_changed)
        on_server_epoch_changed(server_epoch);
}

void RenderClientOfPaintServer::did_hello(RequestID request_id, int peer_pid, GPUEpoch server_epoch, size_t max_texture_size)
{
    set_server_epoch(server_epoch);

    auto maybe_callback = m_pending_hello.get(request_id);
    if (!maybe_callback.has_value())
        return;

    auto callback = move(maybe_callback.value());
    m_pending_hello.remove(request_id);

    callback(HelloResponse {
        .peer_pid = peer_pid,
        .server_epoch = server_epoch,
        .max_texture_size = max_texture_size });
}

void RenderClientOfPaintServer::did_complete(SurfaceID surface_id, ReleaseToken release_token, CompletionStatus status)
{
    if (on_did_complete)
        on_did_complete(surface_id, release_token, status);
}

void RenderClientOfPaintServer::did_complete_offscreen_render(SurfaceID surface_id, RequestID request_id, ImageID image_id, Optional<Gfx::SharedImagePayload> content_image, Optional<Gfx::ShareableBitmap> bitmap)
{
    if (on_did_complete_offscreen_render)
        on_did_complete_offscreen_render(surface_id, request_id, image_id, move(content_image), move(bitmap));
}

void RenderClientOfPaintServer::request_frame_repaint(SurfaceID surface_id)
{
    if (on_request_frame_repaint)
        on_request_frame_repaint(surface_id);
}

void RenderClientOfPaintServer::did_fail_resource_registration(SurfaceID surface_id, ResourceID resource_id, ReleaseToken release_token)
{
    if (is_logging_enabled(LOG_RESOURCE) || is_logging_enabled(LOG_INGRESS)) {
        dbgln("RenderClientOfPaintServer: did_fail_resource_registration {} surface_id={} release_token={}",
            resource_id,
            surface_id,
            release_token);
    }

    if (on_did_fail_resource_registration)
        on_did_fail_resource_registration(surface_id, resource_id, release_token);
}

void RenderClientOfPaintServer::did_create_content_image(RequestID request_id, bool success, ImageID image_id, Optional<Gfx::SharedImagePayload> content_image)
{
    if (is_logging_enabled(LOG_RESOURCE)) {
        Gfx::IntSize size;
        if (content_image.has_value())
            size = content_image->info().size;
        dbgevery(120, "ContentImage"sv, "RenderClientOfPaintServer: did_create_content_image request_id={} success={} image_id={} size={}x{} has_content_image={} server_epoch={}",
            request_id,
            success,
            image_id,
            size.width(),
            size.height(),
            content_image.has_value(),
            m_server_epoch);
    }

    auto maybe_callback = m_pending_create_content_image.get(request_id);
    if (!maybe_callback.has_value())
        return;

    auto callback = move(maybe_callback.value());
    m_pending_create_content_image.remove(request_id);

    callback(CreatedContentImage {
        .success = success,
        .image_id = image_id,
        .content_image = move(content_image),
    });
}

void RenderClientOfPaintServer::did_import_content_image(RequestID request_id, bool success, ImageID image_id)
{
    auto maybe_callback = m_pending_import_content_image.get(request_id);
    if (!maybe_callback.has_value())
        return;

    auto callback = move(maybe_callback.value());
    m_pending_import_content_image.remove(request_id);

    callback(success && image_id != 0);
}

}
