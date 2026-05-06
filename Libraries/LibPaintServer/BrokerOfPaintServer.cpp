/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibPaintServer/BrokerOfPaintServer.h>

namespace WebView {

using namespace PaintServer;

BrokerOfPaintServer::BrokerOfPaintServer(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToBrokerFromPaintServerEndpoint, ToPaintServerFromBrokerEndpoint>(*this, move(transport))
{
    m_presentation_backend = create_presentation_backend();
}

void BrokerOfPaintServer::destroy_presentation_surface(SurfaceID surface_id)
{
    async_destroy_presentation_surface(surface_id);

    if (m_presentation_backend)
        m_presentation_backend->clear_surface(surface_id);
}

void BrokerOfPaintServer::ensure_broker_owned_presentation_buffers(SurfaceID surface_id, Gfx::IntSize size)
{
    if (!m_presentation_backend)
        return;
    m_presentation_backend->ensure_broker_owned_presentation_buffers(*this, surface_id, size);
}

bool BrokerOfPaintServer::has_pool_image(SurfaceID surface_id, ImageID image_id) const
{
    if (!m_presentation_backend)
        return false;
    return m_presentation_backend->has_pool_image(surface_id, image_id);
}

Optional<void*> BrokerOfPaintServer::platform_surface_handle_for_image(SurfaceID surface_id, ImageID image_id)
{
    if (!m_presentation_backend)
        return {};
    return m_presentation_backend->platform_surface_handle_for_image(surface_id, image_id);
}

Optional<LinuxDmaBufPresentationBuffer> BrokerOfPaintServer::clone_linux_dmabuf_presentation_buffer(SurfaceID surface_id, ImageID image_id) const
{
    if (!m_presentation_backend)
        return {};
    return m_presentation_backend->clone_linux_dmabuf_presentation_buffer(surface_id, image_id);
}

RefPtr<Gfx::Bitmap const> BrokerOfPaintServer::bitmap_for_presentation_image(SurfaceID surface_id, ImageID image_id) const
{
    if (!m_presentation_backend)
        return nullptr;
    return m_presentation_backend->bitmap_for_presentation_image(surface_id, image_id);
}

void BrokerOfPaintServer::die()
{
    auto on_death_callback = move(on_death);
    if (on_death_callback)
        on_death_callback();
}

void BrokerOfPaintServer::server_epoch_changed(GPUEpoch server_epoch)
{
    if (on_server_epoch_changed)
        on_server_epoch_changed(server_epoch);
}

void BrokerOfPaintServer::hello(Function<void(HelloResponse)> callback)
{
    RequestID request_id = m_next_request_id++;

    if (callback)
        m_pending_hello.set(request_id, move(callback));

    async_hello(request_id, ClientKind::Broker, Core::System::getpid());
}

void BrokerOfPaintServer::did_hello(RequestID request_id, int peer_pid, GPUEpoch server_epoch, size_t max_texture_size)
{
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

void BrokerOfPaintServer::capabilities(size_t max_texture_size)
{
    if (on_capabilities)
        on_capabilities(max_texture_size);
}

void BrokerOfPaintServer::frame_ready(SurfaceID surface_id, FrameID present_id, ImageID image_id, Gfx::IntSize frame_size)
{
    if (on_frame_ready)
        on_frame_ready(surface_id, present_id, image_id, frame_size);
}

}
