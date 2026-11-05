/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/AnimatedDecodedImageData.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

// A multi-frame image, which the compositor moves through by itself under the id of the first frame here. The display
// lists draw that frame, so they stay the same as the image advances, and the main thread has no part in it.
class AnimatedBitmapDecodedImageData final : public AnimatedDecodedImageData {
    GC_CELL(AnimatedBitmapDecodedImageData, AnimatedDecodedImageData);
    GC_DECLARE_ALLOCATOR(AnimatedBitmapDecodedImageData);
    friend class Web::Internals::Internals;

public:
    static GC::Ref<AnimatedBitmapDecodedImageData> create(
        DOM::Document&,
        i64 session_id,
        u32 loop_count,
        Gfx::ColorSpace,
        Vector<u32> durations,
        NonnullRefPtr<Gfx::Bitmap> first_bitmap);

    virtual ~AnimatedBitmapDecodedImageData() override;
    virtual void finalize() override;

    virtual Optional<Gfx::DecodedImageFrame> default_frame(Gfx::IntSize = {}) const override;
    virtual Optional<Gfx::DecodedImageFrame> current_frame(Gfx::IntSize = {}) const override;

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;

    virtual Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const override;

private:
    AnimatedBitmapDecodedImageData(
        i64 session_id,
        u32 frame_count,
        u32 loop_count,
        Gfx::DecodedImageFrame first_frame,
        GC::Ref<DOM::DocumentObserver>);

    virtual size_t external_memory_size() const override;

    virtual void start_animation() override;
    virtual void stop_animation() override;

    Compositor::AnimatedImageState state_for_testing() const;

    i64 m_session_id;
    u32 m_frame_count;
    u32 m_loop_count;
    Gfx::DecodedImageFrame m_first_frame;
    // NB: The compositor replaces the pixels of m_first_frame under its id, so what wants the default image rather than
    //     the animation, such as feImage, gets the same pixels under an id of their own.
    Gfx::DecodedImageFrame m_default_frame;
    u64 m_image_decoder_client_generation { 0 };
    // NB: A page with no compositor host, such as that of an SVG image, shows the first frame.
    WeakPtr<Compositor::CompositorHost> m_compositor_host;
};

}
