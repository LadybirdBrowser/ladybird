/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <Compositor/PresentableSurface.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
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

// Where the window server can show the content of a media layer by itself, in viewport pixels.
struct WindowServerPlacement {
    Gfx::IntRect content_rect;
    Gfx::AffineTransform content_to_viewport;
    Gfx::FloatRect clip_rect;
    Optional<RemoteSublayer::RoundedClip> rounded_clip;
    Gfx::ScalingMode scaling_mode { Gfx::ScalingMode::Bilinear };
};

}
