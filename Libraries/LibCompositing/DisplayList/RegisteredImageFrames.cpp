/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/DisplayList/RegisteredImageFrames.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/Bitmap.h>

namespace Compositing {

// Coarse by design: the verdict only must tell line art from photos; a large image shouldn't pay per-pixel to be asked.
static constexpr size_t max_sampled_pixels = 1000;

static bool classify_image_frame_for_force_dark(Gfx::DecodedImageFrame const& frame)
{
    auto bitmap = frame.bitmap_sharing_pixels();
    auto size = bitmap->size();
    if (size.is_empty())
        return false;

    auto total = static_cast<double>(size.width()) * static_cast<double>(size.height());
    auto step = max(1, static_cast<int>(AK::ceil(AK::sqrt(total / static_cast<double>(max_sampled_pixels)))));

    Vector<u32> opaque_samples;
    size_t transparent_count = 0;
    size_t sampled_count = 0;
    for (int y = 0; y < size.height(); y += step) {
        for (int x = 0; x < size.width(); x += step) {
            auto color = bitmap->get_pixel(x, y);
            sampled_count++;
            // A pixel this sheer says something about the image's shape rather than its palette — so it's counted
            // toward transparency, but kept out of the palette samples.
            if (color.alpha() < 128) {
                transparent_count++;
                continue;
            }
            opaque_samples.append(color.value());
        }
    }
    if (sampled_count == 0)
        return false;

    auto transparency_ratio = static_cast<float>(transparent_count) / static_cast<float>(sampled_count);
    return Compositing::RustFFI::ladybird_web_force_dark_should_filter_image(
        opaque_samples.data(), opaque_samples.size(), transparency_ratio);
}

static u64 s_next_generation_of_replaced_pixels = 1;

RegisteredImageFrame::RegisteredImageFrame(Gfx::DecodedImageFrame frame)
    : m_frame(move(frame))
{
}

bool RegisteredImageFrame::should_force_dark() const
{
    // Classifying costs a walk over the pixels and the answer never changes for a given frame — so it's worked out on
    // first use, and kept.
    if (!m_should_force_dark.has_value())
        m_should_force_dark = classify_image_frame_for_force_dark(m_frame);
    return m_should_force_dark.value();
}

void RegisteredImageFrame::replace_pixels(Gfx::DecodedImageFrame frame)
{
    // NB: Whether force-dark filters the image stays as it was, so that it does not flip between the frames of one
    //     image.
    m_frame = move(frame);
    m_generation_of_replaced_pixels = s_next_generation_of_replaced_pixels++;
}

void RegisteredImageFrames::register_image_frame(DisplayListImageFrameResource resource)
{
    m_image_frames_by_id.set(resource.id.value(), make_ref_counted<RegisteredImageFrame>(move(resource.frame)));
}

void RegisteredImageFrames::unregister_image_frame(ImageFrameResourceId id)
{
    m_image_frames_by_id.remove(id.value());
}

RefPtr<RegisteredImageFrame> RegisteredImageFrames::image_frame(ImageFrameResourceId id) const
{
    auto image_frame = m_image_frames_by_id.find(id.value());
    if (image_frame == m_image_frames_by_id.end())
        return nullptr;
    return image_frame->value;
}

}
