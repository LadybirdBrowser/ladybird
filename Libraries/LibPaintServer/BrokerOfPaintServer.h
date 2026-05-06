/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <LibPaintServer/Presentation.h>
#include <LibPaintServer/ToBrokerFromPaintServerEndpoint.h>
#include <LibPaintServer/ToPaintServerFromBrokerEndpoint.h>
#include <LibPaintServer/Types.h>

namespace WebView {

class BrokerOfPaintServer final
    : public IPC::ConnectionToServer<ToBrokerFromPaintServerEndpoint, ToPaintServerFromBrokerEndpoint>
    , public ToBrokerFromPaintServerEndpoint {
    C_OBJECT_ABSTRACT(BrokerOfPaintServer);

public:
    using ConnectionID = PaintServer::ConnectionID;
    using FrameID = PaintServer::FrameID;
    using GPUEpoch = PaintServer::GPUEpoch;
    using ImageID = PaintServer::ImageID;
    using RequestID = PaintServer::RequestID;
    using SurfaceID = PaintServer::SurfaceID;

    explicit BrokerOfPaintServer(NonnullOwnPtr<IPC::Transport>);

    void set_pid(pid_t pid) { m_paint_server_pid = pid; }
    pid_t pid() const { return m_paint_server_pid; }

    struct HelloResponse {
        int peer_pid { -1 };
        GPUEpoch server_epoch { 0 };
        size_t max_texture_size { 0 };
    };

    void hello(Function<void(HelloResponse)> callback = {});

    void destroy_presentation_surface(SurfaceID surface_id);
    void set_prefer_cpu_mappable_presentation_buffers(bool prefer_cpu_mappable_presentation_buffers) { m_prefer_cpu_mappable_presentation_buffers = prefer_cpu_mappable_presentation_buffers; }
    bool prefer_cpu_mappable_presentation_buffers() const { return m_prefer_cpu_mappable_presentation_buffers; }
    void ensure_broker_owned_presentation_buffers(SurfaceID surface_id, Gfx::IntSize size);
    bool has_pool_image(SurfaceID surface_id, ImageID image_id) const;
    Optional<void*> platform_surface_handle_for_image(SurfaceID surface_id, ImageID image_id);
    Optional<LinuxDmaBufPresentationBuffer> clone_linux_dmabuf_presentation_buffer(SurfaceID surface_id, ImageID image_id) const;
    RefPtr<Gfx::Bitmap const> bitmap_for_presentation_image(SurfaceID surface_id, ImageID image_id) const;

    Function<void()> on_death;
    Function<void(GPUEpoch server_epoch)> on_server_epoch_changed;
    Function<void(size_t max_texture_size)> on_capabilities;
    Function<void(SurfaceID surface_id, FrameID present_id, ImageID image_id, Gfx::IntSize frame_size)> on_frame_ready;

private:
    void die() override;
    void server_epoch_changed(GPUEpoch server_epoch) override;
    void did_hello(RequestID request_id, int peer_pid, GPUEpoch server_epoch, size_t max_texture_size) override;
    void capabilities(size_t max_texture_size) override;
    void frame_ready(SurfaceID surface_id, FrameID present_id, ImageID image_id, Gfx::IntSize frame_size) override;

    pid_t m_paint_server_pid { -1 };
    OwnPtr<Presentation> m_presentation_backend;
    bool m_prefer_cpu_mappable_presentation_buffers { false };
    RequestID m_next_request_id { 1 };
    HashMap<RequestID, Function<void(HelloResponse)>> m_pending_hello;
};

}
