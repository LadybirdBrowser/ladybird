/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/Math.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositing {

struct DisplayListStoredImageFrameResource {
    AK_ALLOC_WITH_KMALLOC;

    explicit DisplayListStoredImageFrameResource(Gfx::DecodedImageFrame frame)
        : frame(move(frame))
    {
    }

    Gfx::DecodedImageFrame frame;
    // Classifying costs a walk over the pixels and the answer never changes for a given frame — so it's worked out on
    // first use, and kept.
    mutable Optional<bool> force_dark_should_filter;
};

struct DisplayListStoredVideoSinkResource {
    AK_ALLOC_WITH_KMALLOC;

    RefPtr<Media::DisplayingVideoSink> sink;
};

bool DisplayListResourceSet::is_empty() const
{
    return fonts.is_empty()
        && image_frames.is_empty()
        && video_sinks.is_empty()
        && display_lists.is_empty();
}

void DisplayListResourceSet::include(DisplayListResourceSet const& other)
{
    for (auto id : other.fonts)
        fonts.set(id, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : other.image_frames)
        image_frames.set(id, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : other.video_sinks)
        video_sinks.set(id, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : other.display_lists)
        display_lists.set(id, AK::HashSetExistingEntryBehavior::Keep);
}

DisplayListResourceStorage::DisplayListResourceStorage() = default;
DisplayListResourceStorage::DisplayListResourceStorage(DisplayListResourceStorage&&) = default;
DisplayListResourceStorage& DisplayListResourceStorage::operator=(DisplayListResourceStorage&&) = default;
DisplayListResourceStorage::~DisplayListResourceStorage() = default;

FontResourceId DisplayListResourceStorage::add_font(Gfx::Font const& font)
{
    m_has_resources_added_since_last_retain = true;
    auto id = font.id();
    m_fonts.ensure(id, [&]() -> NonnullRefPtr<Gfx::Font const> { return font; });
    return { id };
}

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

bool DisplayListResourceStorage::image_frame_should_force_dark(ImageFrameResourceId id) const
{
    auto stored = m_image_frames.get(id.value());
    if (!stored.has_value())
        return false;
    auto const& resource = *stored.value();
    if (!resource.force_dark_should_filter.has_value())
        resource.force_dark_should_filter = classify_image_frame_for_force_dark(resource.frame);
    return resource.force_dark_should_filter.value();
}

ImageFrameResourceId DisplayListResourceStorage::add_image_frame(Gfx::DecodedImageFrame const& frame)
{
    m_has_resources_added_since_last_retain = true;
    auto id = frame.id();
    m_image_frames.ensure(id, [&] { return make<DisplayListStoredImageFrameResource>(frame); });
    return { id };
}

VideoSinkResourceId DisplayListResourceStorage::add_video_sink(VideoSinkResourceId id, Media::VideoSinkHandle sink_handle)
{
    m_has_resources_added_since_last_retain = true;
    m_video_sink_handles.set(id.value(), sink_handle, AK::HashSetExistingEntryBehavior::Keep);
    return id;
}

void DisplayListResourceStorage::set_font(FontResourceId id, NonnullRefPtr<Gfx::Font const> font)
{
    m_has_resources_added_since_last_retain = true;
    m_fonts.set(id.value(), move(font));
}

void DisplayListResourceStorage::set_image_frame(ImageFrameResourceId id, Gfx::DecodedImageFrame frame)
{
    m_has_resources_added_since_last_retain = true;
    m_image_frames.set(id.value(), make<DisplayListStoredImageFrameResource>(move(frame)));
}

void DisplayListResourceStorage::replace_image_frame_pixels(ImageFrameResourceId id, Gfx::DecodedImageFrame frame)
{
    auto stored = m_image_frames.get(id.value());
    VERIFY(stored.has_value());
    // NB: Whether force-dark filters the image stays as it was, so that it does not flip between the frames of one
    //     image.
    stored.value()->frame = move(frame);
    m_image_frame_pixel_generations.set(id.value(), m_next_image_frame_pixel_generation++);
}

Gfx::DecodedImageFrame const& DisplayListResourceStorage::image_frame(ImageFrameResourceId id) const
{
    return m_image_frames.get(id.value()).value()->frame;
}

DisplayListResourceTransaction DisplayListResourceStorage::create_transaction(
    DisplayListResourceSet const& previous,
    DisplayListResourceSet const& current) const
{
    DisplayListResourceTransaction transaction;

    for (auto id : current.fonts) {
        if (!previous.fonts.contains(id))
            transaction.fonts.append({ id, font(id) });
    }
    for (auto id : current.image_frames) {
        if (!previous.image_frames.contains(id))
            transaction.image_frames.append({ id, image_frame(id) });
    }
    for (auto id : current.video_sinks) {
        if (previous.video_sinks.contains(id))
            continue;
        if (auto sink_handle = video_sink_handle(id); sink_handle.has_value())
            transaction.video_sinks.append({ id, *sink_handle });
    }

    for (auto id : previous.fonts) {
        if (!current.fonts.contains(id))
            transaction.font_ids_to_remove.append(id);
    }
    for (auto id : previous.image_frames) {
        if (!current.image_frames.contains(id))
            transaction.image_frame_ids_to_remove.append(id);
    }
    for (auto id : previous.video_sinks) {
        if (!current.video_sinks.contains(id))
            transaction.video_sink_ids_to_remove.append(id);
    }
    for (auto id : previous.display_lists) {
        if (!current.display_lists.contains(id))
            transaction.display_list_ids_to_remove.append(id);
    }
    return transaction;
}

DisplayListResourceSet DisplayListResourceStorage::apply_transaction(DisplayListResourceTransaction&& transaction)
{
    DisplayListResourceSet removed_resources;
    m_has_resources_added_since_last_retain = true;
    for (auto& font : transaction.fonts)
        set_font(font.id, move(font.font));
    for (auto& frame : transaction.image_frames) {
        if (m_image_frames.contains(frame.id.value()))
            removed_resources.image_frames.set(frame.id);
        set_image_frame(frame.id, move(frame.frame));
    }
    for (auto& video_sink : transaction.video_sinks)
        add_video_sink(video_sink.id, video_sink.sink_handle);

    for (auto id : transaction.font_ids_to_remove) {
        if (m_fonts.remove(id.value()))
            removed_resources.fonts.set(id);
    }
    for (auto id : transaction.image_frame_ids_to_remove) {
        m_image_frame_pixel_generations.remove(id.value());
        if (m_image_frames.remove(id.value()))
            removed_resources.image_frames.set(id);
    }
    for (auto id : transaction.video_sink_ids_to_remove) {
        auto removed_handle = m_video_sink_handles.remove(id.value());
        auto removed_sink = m_video_sinks.remove(id.value());
        if (removed_handle || removed_sink)
            removed_resources.video_sinks.set(id);
    }
    return removed_resources;
}

DisplayListResourceSet DisplayListResourceStorage::retain_only(DisplayListResourceSet const& resource_set)
{
    DisplayListResourceSet removed_resources;
    m_fonts.remove_all_matching([&](auto id, auto const&) {
        if (resource_set.fonts.contains(FontResourceId { id }))
            return false;
        removed_resources.fonts.set(FontResourceId { id });
        return true;
    });
    m_image_frames.remove_all_matching([&](auto id, auto const&) {
        if (resource_set.image_frames.contains(ImageFrameResourceId { id }))
            return false;
        m_image_frame_pixel_generations.remove(id);
        removed_resources.image_frames.set(ImageFrameResourceId { id });
        return true;
    });
    auto remove_video_resource = [&](auto id) {
        if (resource_set.video_sinks.contains(VideoSinkResourceId { id }))
            return false;
        removed_resources.video_sinks.set(VideoSinkResourceId { id });
        return true;
    };
    m_video_sink_handles.remove_all_matching([&](auto id, auto const&) { return remove_video_resource(id); });
    m_video_sinks.remove_all_matching([&](auto id, auto const&) { return remove_video_resource(id); });
    m_has_resources_added_since_last_retain = false;
    return removed_resources;
}

void DisplayListResourceStorage::set_video_sink(VideoSinkResourceId id, RefPtr<Media::DisplayingVideoSink> sink)
{
    m_has_resources_added_since_last_retain = true;
    m_video_sinks.ensure(id.value(), [] { return make<DisplayListStoredVideoSinkResource>(); })->sink = move(sink);
}

RefPtr<Media::DisplayingVideoSink const> DisplayListResourceStorage::video_sink(VideoSinkResourceId id) const
{
    auto stored = m_video_sinks.find(id.value());
    if (stored == m_video_sinks.end())
        return nullptr;
    return stored->value->sink;
}

}
