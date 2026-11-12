/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <Compositor/PresentableSurface.h>
#include <Compositor/RemoteLayer.h>
#include <Compositor/WindowServer.h>
#include <LibCore/Forward.h>
#include <LibCore/IOSurface.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

class CoreAnimationSurface final : public WindowServerSurface {
public:
    // Over an IOSurface that something else owns.
    explicit CoreAnimationSurface(Gfx::SharedImageBuffer const&);
    explicit CoreAnimationSurface(NonnullOwnPtr<Gfx::SharedImageBuffer>);
    virtual ~CoreAnimationSurface() override;

    Core::IOSurfaceHandle const& iosurface() const { return m_iosurface; }
    virtual bool may_be_read_by_window_server() const override { return m_iosurface.is_in_use(); }

private:
    OwnPtr<Gfx::SharedImageBuffer> m_owned_shared_image_buffer;
    Core::IOSurfaceHandle m_iosurface;
};

class CoreAnimationWindowServer final : public WindowServer {
public:
    // None where the layer trees that another process shows by the ID of their context are not available.
    static RefPtr<CoreAnimationWindowServer> try_create();
    static ErrorOr<NonnullRefPtr<PresentableSurface>> try_create_presentable_surface_over_new_iosurface(Gfx::IntSize, Gfx::SkiaBackendContext&);

    virtual OwnPtr<RemoteLayer> try_create_remote_layer(u64 remote_layer_id) override;
    virtual ErrorOr<NonnullRefPtr<PresentableSurface>> try_create_presentable_surface(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&) override;
    virtual FrameHandOff frame_hand_off() const override { return FrameHandOff::OnceGpuWorkIsScheduled; }
    virtual void hold_changes_on_screen() override;
    virtual void release_changes_held_off_screen() override;

private:
    CoreAnimationWindowServer() = default;
};

// A CoreAnimation layer tree that this process commits frames into, and that another process shows in a view of its
// own by the ID of the tree's context.
class CoreAnimationRemoteLayer final : public RemoteLayer {
public:
    static OwnPtr<CoreAnimationRemoteLayer> try_create();
    virtual ~CoreAnimationRemoteLayer() override;

    virtual Compositing::RemoteLayerHostHandle host_handle() const override;
    virtual bool can_show(WindowServerPlacement const&, Content const&, double device_pixel_ratio) const override;
    virtual bool can_show_high_dynamic_range() const override { return true; }
    virtual void show_frame(Frame) override;
    virtual void place_frame(Optional<Gfx::IntSize> fit_within) override;

private:
    CoreAnimationRemoteLayer() = default;

    struct ShownSublayer {
        void* clip_layer { nullptr };
        void* rounded_clip_layer { nullptr };
        void* content_layer { nullptr };
        Variant<Empty, NonnullRefPtr<Media::VideoFrame>, PresentableSurface::Lease> content;
    };

    void lay_out_frame(Optional<Gfx::IntSize> fit_within);
    void show_sublayers(Vector<RemoteSublayer>&);
    void use_content_layer_of_kind_for(ShownSublayer&, RemoteSublayer const&);
    void update_shown_sublayer(ShownSublayer&, RemoteSublayer&);
    void show_content(ShownSublayer&, RemoteSublayer&);
    void stop_showing_content(ShownSublayer&);
    void stop_showing_sublayer(ShownSublayer&);
    void keep_video_frame_until_window_server_lets_go(NonnullRefPtr<Media::VideoFrame>);
    void release_video_frames_window_server_let_go_of();

    void* m_context { nullptr };
    void* m_root_layer { nullptr };
    void* m_frame_layer { nullptr };
    void* m_content_layer { nullptr };
    Gfx::IntSize m_painted_size;
    double m_device_pixel_ratio { 1.0 };
    HashMap<u64, ShownSublayer> m_shown_sublayers;
    // A decoder decodes into the surface of a frame again once nothing holds the frame, so a replaced frame is held
    // until the window server can no longer be reading it. Its surface reads as in use for as long as the decoder's
    // pixel buffer lives, which is as long as the frame does, so that cannot tell.
    struct ReplacedVideoFrame {
        NonnullRefPtr<Media::VideoFrame> frame;
        MonotonicTime replaced_at;
    };
    Vector<ReplacedVideoFrame> m_replaced_video_frames;
    RefPtr<Core::Timer> m_video_frame_release_timer;
    u32 m_core_animation_context_id { 0 };
};

}
