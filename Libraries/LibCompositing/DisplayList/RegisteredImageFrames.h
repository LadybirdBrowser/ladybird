/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/DisplayListResources.h>
#include <LibCompositing/Export.h>
#include <LibGfx/DecodedImageFrame.h>

namespace Compositing {

// An image frame that a client registered. Whatever draws it holds it, so it outlasts its registration for as long as
// it is drawn.
class COMPOSITING_API RegisteredImageFrame final : public RefCounted<RegisteredImageFrame> {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit RegisteredImageFrame(Gfx::DecodedImageFrame);

    Gfx::DecodedImageFrame const& frame() const { return m_frame; }
    // Whether force-dark should invert this image, worked out once and cached.
    bool should_force_dark() const;

    // Shows other pixels under the id the display lists keep drawing, as an animated image moves to its next frame.
    void replace_pixels(Gfx::DecodedImageFrame);
    // A stamp for the pixels that replaced the ones the frame had before, or none for a frame that still has the
    // pixels it was registered with. No two replacements share a stamp, so an equal stamp always means the same
    // pixels.
    Optional<u64> generation_of_replaced_pixels() const { return m_generation_of_replaced_pixels; }

private:
    Gfx::DecodedImageFrame m_frame;
    mutable Optional<bool> m_should_force_dark;
    Optional<u64> m_generation_of_replaced_pixels;
};

class COMPOSITING_API RegisteredImageFrames {
public:
    void register_image_frame(DisplayListImageFrameResource);
    void unregister_image_frame(ImageFrameResourceId);

    RefPtr<RegisteredImageFrame> image_frame(ImageFrameResourceId) const;

private:
    HashMap<u64, NonnullRefPtr<RegisteredImageFrame>> m_image_frames_by_id;
};

}
