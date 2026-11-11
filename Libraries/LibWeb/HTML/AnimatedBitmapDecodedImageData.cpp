/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AnimatedBitmapDecodedImageData);

GC::Ref<AnimatedBitmapDecodedImageData> AnimatedBitmapDecodedImageData::create(
    DOM::Document& document,
    i64 session_id,
    u32 loop_count,
    Gfx::ColorSpace color_space,
    Vector<u32> durations,
    Gfx::DecodedImageFrame first_frame)
{
    auto document_observer = GC::Heap::the().allocate<DOM::DocumentObserver>(document);
    auto size = first_frame.size();
    auto data = GC::Heap::the().allocate<AnimatedBitmapDecodedImageData>(
        session_id, durations.size(), loop_count, move(first_frame), document_observer);

    data->m_image_decoder_client_generation = Platform::ImageCodecPlugin::the().client_generation();

    auto& page = document.page();
    page.ensure_compositor_host();
    if (page.has_compositor_host()) {
        auto& compositor_host = page.compositor_host();
        compositor_host.add_animated_image(data->m_first_frame.id(), {
                                                                         .session_id = session_id,
                                                                         .size = size,
                                                                         .color_space = move(color_space),
                                                                         .frame_durations_in_milliseconds = move(durations),
                                                                         .loop_count = loop_count,
                                                                         .image_decoder_client_generation = data->m_image_decoder_client_generation,
                                                                     });
        data->m_compositor_host = compositor_host.make_weak_ptr<Compositor::CompositorHost>();
    }
    return data;
}

AnimatedBitmapDecodedImageData::AnimatedBitmapDecodedImageData(
    i64 session_id,
    u32 frame_count,
    u32 loop_count,
    Gfx::DecodedImageFrame first_frame,
    GC::Ref<DOM::DocumentObserver> document_observer)
    : AnimatedDecodedImageData(document_observer)
    , m_session_id(session_id)
    , m_frame_count(frame_count)
    , m_loop_count(loop_count)
    , m_first_frame(move(first_frame))
    , m_default_frame(m_first_frame.with_new_id())
{
}

AnimatedBitmapDecodedImageData::~AnimatedBitmapDecodedImageData() = default;

size_t AnimatedBitmapDecodedImageData::external_memory_size() const
{
    return m_first_frame.size_in_bytes();
}

void AnimatedBitmapDecodedImageData::finalize()
{
    Base::finalize();
    if (m_compositor_host)
        m_compositor_host->remove_animated_image(m_first_frame.id());
    // NB: Session ids start over with each decoder, so a replacement may have given this id to another image.
    auto& image_codec = Platform::ImageCodecPlugin::the();
    if (image_codec.client_generation() == m_image_decoder_client_generation)
        image_codec.stop_animation_decode(m_session_id);
}

void AnimatedBitmapDecodedImageData::start_animation()
{
    if (m_compositor_host)
        m_compositor_host->start_animated_image(m_first_frame.id());
}

void AnimatedBitmapDecodedImageData::stop_animation()
{
    if (m_compositor_host)
        m_compositor_host->stop_animated_image(m_first_frame.id());
}

Compositor::AnimatedImageState AnimatedBitmapDecodedImageData::state_for_testing() const
{
    if (m_compositor_host)
        return m_compositor_host->animated_image_state_for_testing(m_first_frame.id());
    return {};
}

Optional<Gfx::DecodedImageFrame> AnimatedBitmapDecodedImageData::default_frame(Gfx::IntSize) const
{
    // FIXME: This should account for if the file specifies a default frame other than the first frame e.g. the
    //        "static image" in APNG or the "primary image item" in AVIF.
    return m_default_frame;
}

Optional<Gfx::DecodedImageFrame> AnimatedBitmapDecodedImageData::current_frame(Gfx::IntSize) const
{
    return m_first_frame;
}

Optional<Gfx::Color> AnimatedBitmapDecodedImageData::color_if_single_pixel_bitmap() const
{
    // NB: The compositor moves through the frames, so the first frame's color says nothing about the others.
    return {};
}

Optional<CSSPixels> AnimatedBitmapDecodedImageData::intrinsic_width() const
{
    return m_first_frame.width();
}

Optional<CSSPixels> AnimatedBitmapDecodedImageData::intrinsic_height() const
{
    return m_first_frame.height();
}

Optional<CSSPixelFraction> AnimatedBitmapDecodedImageData::intrinsic_aspect_ratio() const
{
    return CSSPixels(m_first_frame.width()) / CSSPixels(m_first_frame.height());
}

Optional<Painting::ImagePaint> AnimatedBitmapDecodedImageData::image_paint(Painting::ImagePaintRequest const&) const
{
    return Painting::ImagePaint { Painting::ImagePaint::DecodedFrame { .frame = m_first_frame, .natural_size = m_first_frame.size() } };
}

}
