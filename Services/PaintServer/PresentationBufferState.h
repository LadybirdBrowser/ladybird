/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class PresentationBufferState {
public:
    struct PresentedImageInfo {
        u64 image_id { 0 };
        Gfx::IntSize frame_size;
    };

    struct PresentReleaseResult {
        u64 image_id { 0 };
        bool had_mapping { false };
    };

    void clear_surface(SurfaceID surface_id);
    void add_surface_buffer(SurfaceID surface_id, ImageID image_id);
    Optional<u64> reserve_next_buffer(SurfaceID surface_id, u64 present_id);
    void release_submit_reservation(u64 present_id, Optional<u64> reserved_image_id);
    PresentReleaseResult did_present_or_released(u64 present_id);
    void stamp_image(ImageID image_id, Gfx::IntSize frame_size);
    Optional<Gfx::IntSize> stamped_frame_size_for_image(ImageID image_id) const;

private:
    HashMap<SurfaceID, Vector<u64>> m_presentation_buffer_pools;
    HashMap<SurfaceID, size_t> m_presentation_buffer_next_index;
    HashMap<u64, u64> m_present_id_to_image_id;
    HashMap<u64, Gfx::IntSize> m_image_frame_sizes;
};

}
