/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <Compositor/PresentableSurface.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/PathData.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/WindingRule.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

// Content that the window server shows of a frame by itself: a video frame or a surface placed and clipped as drawing it
// into the frame would have placed and clipped it, or a tile or a fill of a painted layer in a group that places and
// clips every sublayer in it. Geometry is in device pixels.
struct RemoteSublayer {
    struct RoundedClip {
        Gfx::FloatRect rect;
        float radius { 0 };
        bool rounds_top_left { false };
        bool rounds_top_right { false };
        bool rounds_bottom_right { false };
        bool rounds_bottom_left { false };
    };

    // A path that clips the content, with a copy of its contours, as the path the copy is of may be gone by the time
    // the frame is shown. The contours are in a space of their own, which the transform maps into the frame.
    struct PathClip {
        // Tells the path apart from every other path of the process, so that a frame that clips by the same path as
        // the one before does not make its outline again.
        u64 path_identity { 0 };
        Vector<Gfx::PathVerb> verbs;
        Vector<Gfx::FloatPoint> points;
        Vector<float> conic_weights;
        Gfx::WindingRule winding_rule { Gfx::WindingRule::Nonzero };
        Gfx::AffineTransform path_to_frame;
    };

    // Holds the sublayers that name it as their group.
    struct Group {
        // From the pixels of the group to those of the group it is in, or to those of the frame.
        Gfx::AffineTransform to_parent;
        // Clips the sublayers of the group to this rect of its own pixels, with its corners rounded by the radii.
        Optional<Gfx::FloatRect> clip_rect;
        Gfx::CornerRadii corner_radii;
        // Composites the sublayers of the group at this opacity, once they are composited with each other.
        float opacity { 1 };
    };

    struct Tile {
        PresentableSurface::Lease surface;
        // What of the surface shows at the content rect. Around it, the surface of a tile holds a border of what lies
        // beside the tile, which a filter blends the tile's edges with where the tile is shown scaled.
        Gfx::IntRect rect_in_surface;
        bool every_pixel_is_opaque { false };
    };

    // What the window server shows below the sublayer, within a rect of the pixels of its group, run through filters.
    struct Backdrop {
        struct Filter {
            enum class Kind : u8 {
                Blur,
                ColorMatrix,
            };
            Kind kind { Kind::Blur };
            // In device pixels.
            float blur_standard_deviation { 0 };
            // Row-major, with the offsets in the fifth column, for unpremultiplied colors from 0 to 1.
            Array<float, 20> color_matrix {};

            bool operator==(Filter const&) const = default;
        };

        Gfx::FloatRect region;
        // Applied in order.
        Vector<Filter, 3> filters;

        bool operator==(Backdrop const&) const = default;
    };

    // Tells a sublayer apart from the others from one frame to the next, so that a frame changes only what moved.
    u64 identity { 0 };
    // The index, among the sublayers of the frame, of the group the sublayer is in, which comes before it. Sublayers of
    // one group, and those of the frame itself, which have none, are shown bottom to top.
    Optional<size_t> group_index;
    Variant<NonnullRefPtr<Media::VideoFrame>, PresentableSurface::Lease, Group, Tile, Gfx::Color, Backdrop> content;
    // Where a video frame or a surface is drawn, in its own space, which the transform maps into the frame. Where a
    // tile or a color is shown, in the pixels of its group.
    Gfx::IntRect content_rect;
    // The transform, the clips and the scaling mode place a video frame or a surface only.
    Gfx::AffineTransform content_to_frame;
    Gfx::FloatRect clip_rect;
    Optional<RoundedClip> rounded_clip;
    Optional<PathClip> path_clip;
    Gfx::ScalingMode scaling_mode { Gfx::ScalingMode::Bilinear };
};

// Where the window server can show the content of a media layer by itself, in viewport pixels.
struct WindowServerPlacement {
    Gfx::IntRect content_rect;
    Gfx::AffineTransform content_to_viewport;
    Gfx::FloatRect clip_rect;
    Optional<RemoteSublayer::RoundedClip> rounded_clip;
    bool is_clipped_by_path { false };
    Gfx::ScalingMode scaling_mode { Gfx::ScalingMode::Bilinear };
};

}
