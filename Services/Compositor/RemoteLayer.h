/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Platform.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <Compositor/PresentableSurface.h>
#include <LibCore/Forward.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

// Content that the window server shows above the frame as it is, placed and clipped as drawing it into the frame would
// have placed and clipped it. Geometry is in device pixels.
struct RemoteSublayer {
    struct RoundedClip {
        Gfx::FloatRect rect;
        float radius { 0 };
        bool rounds_top_left { false };
        bool rounds_top_right { false };
        bool rounds_bottom_right { false };
        bool rounds_bottom_left { false };
    };

    // Tells a sublayer apart from the others from one frame to the next, so that a frame changes only what moved.
    u64 identity { 0 };
    Variant<NonnullRefPtr<Media::VideoFrame>, PresentableSurface::Lease> content;
    // Where the content is drawn, in its own space, which the transform maps into the frame.
    Gfx::IntRect content_rect;
    Gfx::AffineTransform content_to_frame;
    Gfx::FloatRect clip_rect;
    Optional<RoundedClip> rounded_clip;
    Gfx::ScalingMode scaling_mode { Gfx::ScalingMode::Bilinear };
};

// A CoreAnimation layer tree that this process commits frames into, and that another process shows in a view of its
// own by the ID of the tree's context. Frames then reach the screen without passing through that process.
class RemoteLayer {
    AK_MAKE_NONCOPYABLE(RemoteLayer);
    AK_MAKE_NONMOVABLE(RemoteLayer);

public:
    AK_ALLOC_WITH_KMALLOC;

    // None where the platform offers no such layer trees.
    static OwnPtr<RemoteLayer> try_create();
    ~RemoteLayer();

    u32 core_animation_context_id() const { return m_core_animation_context_id; }

    // Shows the part of the buffer that the frame was painted into, at the top left of the layer at its own size, or
    // scaled to fit within the size of the view given, in device pixels, with the sublayers above it, bottom to top.
    // Without a buffer, the frame shown before stays, and only the sublayers change.
    void show_frame(Gfx::SharedImageBuffer const*, Gfx::IntSize painted_size, double device_pixel_ratio, Optional<Gfx::IntSize> fit_within, Vector<RemoteSublayer>);
    // Places the frame on display again, as when the view that it is scaled into changes size.
    void place_frame(Optional<Gfx::IntSize> fit_within);

    // While held, what every remote layer of this process shows changes nowhere on screen, and changes all at once
    // when released.
    static void hold_changes_on_screen();
    static void release_changes_held_off_screen();

private:
    RemoteLayer() = default;

#ifdef AK_OS_MACOS
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
#endif
    u32 m_core_animation_context_id { 0 };
};

#ifndef AK_OS_MACOS
inline OwnPtr<RemoteLayer> RemoteLayer::try_create()
{
    return nullptr;
}

inline RemoteLayer::~RemoteLayer() = default;

inline void RemoteLayer::show_frame(Gfx::SharedImageBuffer const*, Gfx::IntSize, double, Optional<Gfx::IntSize>, Vector<RemoteSublayer>)
{
}

inline void RemoteLayer::place_frame(Optional<Gfx::IntSize>)
{
}

inline void RemoteLayer::hold_changes_on_screen()
{
}

inline void RemoteLayer::release_changes_held_off_screen()
{
}
#endif

}
