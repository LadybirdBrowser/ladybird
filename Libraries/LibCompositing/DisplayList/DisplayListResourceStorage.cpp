/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/Math.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Font/Font.h>
#include <LibMedia/VideoFrame.h>

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

    RefPtr<Media::VideoSink> sink;
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

DisplayListResource::DisplayListResource(NonnullRefPtr<DisplayList> display_list, AccumulatedVisualContextTree visual_context_tree)
    : display_list(move(display_list))
    , visual_context_tree(move(visual_context_tree))
{
}

DisplayListResource::DisplayListResource(NonnullRefPtr<DisplayList const> display_list, AccumulatedVisualContextTree visual_context_tree)
    : display_list(move(display_list))
    , visual_context_tree(move(visual_context_tree))
{
}

DisplayListResource::DisplayListResource(DisplayList const& display_list, AccumulatedVisualContextTree visual_context_tree)
    : display_list(display_list)
    , visual_context_tree(move(visual_context_tree))
{
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
    auto const& bitmap = frame.bitmap();
    auto size = bitmap.size();
    if (size.is_empty())
        return false;

    auto total = static_cast<double>(size.width()) * static_cast<double>(size.height());
    auto step = max(1, static_cast<int>(AK::ceil(AK::sqrt(total / static_cast<double>(max_sampled_pixels)))));

    Vector<u32> opaque_samples;
    size_t transparent_count = 0;
    size_t sampled_count = 0;
    for (int y = 0; y < size.height(); y += step) {
        for (int x = 0; x < size.width(); x += step) {
            auto color = bitmap.get_pixel(x, y);
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

DisplayListResourceId DisplayListResourceStorage::add_display_list(NonnullRefPtr<DisplayList const> display_list, AccumulatedVisualContextTree const& visual_context_tree)
{
    m_has_resources_added_since_last_retain = true;
    auto id = display_list->id();
    m_display_lists.ensure(id, [&] {
        return DisplayListResource { move(display_list), visual_context_tree };
    });
    return { id };
}

DisplayListResourceId DisplayListResourceStorage::add_display_list(DisplayListResource&& resource)
{
    m_has_resources_added_since_last_retain = true;
    auto id = resource.display_list->id();
    m_display_lists.set(id, move(resource), AK::HashSetExistingEntryBehavior::Keep);
    return { id };
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

Gfx::DecodedImageFrame const& DisplayListResourceStorage::image_frame(ImageFrameResourceId id) const
{
    return m_image_frames.get(id.value()).value()->frame;
}

bool DisplayListResourceStorage::display_list_requires_direct_replay(DisplayListResourceId id) const
{
    HashTable<u64> visited_display_lists;
    return nested_display_list_requires_direct_replay(id, visited_display_lists);
}

// Determines whether reusing a rasterization of the display list can produce different pixels than replaying it
// in place on every frame. That is the case when a destination-reading operation (a non-normal blend mode on a
// command or a visual context effect, or a backdrop filter carried by a visual context effect) can see canvas
// content painted before the display list began, or when the list draws live
// content that changes underneath its immutable command stream (video frames are updated in place under a stable
// resource id, canvas surfaces and composited child contexts are resolved at replay time). Destination-reading
// operations enclosed in a save layer recorded within the list only ever read within-list content, so they do
// not require direct replay. The verdict is intrinsic to the list and memoized until the list is removed.
bool DisplayListResourceStorage::nested_display_list_requires_direct_replay(DisplayListResourceId id, HashTable<u64>& visited_display_lists) const
{
    if (auto memoized = m_display_list_requires_direct_replay.get(id.value()); memoized.has_value())
        return *memoized;

    visited_display_lists.set(id.value());
    auto const& list_resource = display_list_resource(id);

    bool requires_direct_replay = list_resource.display_list->requires_direct_replay_without_nested_lists(list_resource.visual_context_tree);
    // NB: A nested list's live content matters at any depth, and its unisolated destination reads matter when its
    //     command is not enclosed in a layer; recursing unconditionally is slightly conservative for the latter.
    for (auto nested_id : list_resource.display_list->referenced_resource_ids().display_lists) {
        if (requires_direct_replay)
            break;
        DisplayListResourceId nested_display_list_id { nested_id };
        if (visited_display_lists.set(nested_display_list_id.value()) != HashSetResult::InsertedNewEntry)
            continue;
        if (has_display_list(nested_display_list_id))
            requires_direct_replay = nested_display_list_requires_direct_replay(nested_display_list_id, visited_display_lists);
    }

    m_display_list_requires_direct_replay.set(id.value(), requires_direct_replay);
    return requires_direct_replay;
}

void DisplayListResourceStorage::collect_referenced_resources(
    DisplayList const& display_list,
    DisplayListResourceSet& referenced_resources) const
{
    auto ids = display_list.referenced_resource_ids();
    for (auto id : ids.fonts)
        referenced_resources.fonts.set(FontResourceId { id }, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : ids.image_frames)
        referenced_resources.image_frames.set(ImageFrameResourceId { id }, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : ids.video_sinks)
        referenced_resources.video_sinks.set(VideoSinkResourceId { id }, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : ids.display_lists)
        add_referenced_display_list(DisplayListResourceId { id }, referenced_resources);
}

void DisplayListResourceStorage::add_referenced_display_list(DisplayListResourceId id, DisplayListResourceSet& referenced_resources) const
{
    if (referenced_resources.display_lists.set(id, AK::HashSetExistingEntryBehavior::Keep) != HashSetResult::InsertedNewEntry)
        return;
    if (!has_display_list(id))
        return;
    collect_referenced_resources(display_list(id), referenced_resources);
    collect_referenced_resources(display_list_visual_context_tree(id), referenced_resources);
}

void DisplayListResourceStorage::collect_referenced_resources(
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceSet& referenced_resources) const
{
    visual_context_tree.for_each_effects_filter_bytes([&](ReadonlyBytes filter_bytes) {
        Gfx::for_each_filter_image_frame_id(filter_bytes, [&](u64 image_id) {
            referenced_resources.image_frames.set(ImageFrameResourceId { image_id }, AK::HashSetExistingEntryBehavior::Keep);
        });
    });
}

DisplayListResourceSet DisplayListResourceStorage::collect_referenced_resources(DisplayList const& display_list) const
{
    DisplayListResourceSet referenced_resources;
    collect_referenced_resources(display_list, referenced_resources);
    return referenced_resources;
}

DisplayListResourceSet DisplayListResourceStorage::collect_referenced_resources(AccumulatedVisualContextTree const& visual_context_tree) const
{
    DisplayListResourceSet referenced_resources;
    collect_referenced_resources(visual_context_tree, referenced_resources);
    return referenced_resources;
}

static ErrorOr<void> validate_display_list_against_visual_context_tree(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree)
{
    if (display_list.compatible_visual_context_tree_structural_epoch() != visual_context_tree.structural_epoch())
        return Error::from_string_literal("Display list was recorded against another visual context tree");
    return validate_display_list_references_live_visual_context_nodes(display_list, visual_context_tree);
}

static ErrorOr<void> validate_resources_are_present(DisplayListResourceStorage const& storage, DisplayListResourceSet const& resources)
{
    for (auto id : resources.fonts) {
        if (!storage.has_font(id))
            return Error::from_string_literal("Display list refers to a font that is not present");
    }
    for (auto id : resources.image_frames) {
        if (!storage.has_image_frame(id))
            return Error::from_string_literal("Display list refers to an image frame that is not present");
    }
    for (auto id : resources.display_lists) {
        if (!storage.has_display_list(id))
            return Error::from_string_literal("Display list refers to a nested display list that is not present");
    }
    return {};
}

ErrorOr<DisplayListResourceTransaction> DisplayListResourceStorage::create_self_contained_transaction(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree) const
{
    auto resources = collect_referenced_resources(display_list);
    resources.include(collect_referenced_resources(visual_context_tree));
    TRY(validate_resources_are_present(*this, resources));
    return create_transaction({}, resources);
}

ErrorOr<void> DisplayListResourceStorage::validate_for_replay(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree) const
{
    TRY(validate_display_list_against_visual_context_tree(display_list, visual_context_tree));
    auto resources = collect_referenced_resources(display_list);
    resources.include(collect_referenced_resources(visual_context_tree));
    TRY(validate_resources_are_present(*this, resources));
    for (auto id : resources.display_lists) {
        auto const& nested_display_list = this->display_list(id);
        TRY(validate_display_list_against_visual_context_tree(nested_display_list, display_list_visual_context_tree(id)));
        // Replay follows nested lists with no depth limit, so a list must not reach itself.
        if (collect_referenced_resources(nested_display_list).display_lists.contains(id))
            return Error::from_string_literal("Nested display list refers to itself");
    }
    return {};
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
    for (auto id : current.display_lists) {
        if (!previous.display_lists.contains(id))
            transaction.display_lists.append({ display_list_resource(id).display_list, display_list_visual_context_tree(id) });
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
    for (auto& display_list : transaction.display_lists)
        add_display_list(move(display_list));

    for (auto id : transaction.font_ids_to_remove) {
        if (m_fonts.remove(id.value()))
            removed_resources.fonts.set(id);
    }
    for (auto id : transaction.image_frame_ids_to_remove) {
        if (m_image_frames.remove(id.value()))
            removed_resources.image_frames.set(id);
    }
    for (auto id : transaction.video_sink_ids_to_remove) {
        auto removed_handle = m_video_sink_handles.remove(id.value());
        auto removed_sink = m_video_sinks.remove(id.value());
        if (removed_handle || removed_sink)
            removed_resources.video_sinks.set(id);
    }
    for (auto id : transaction.display_list_ids_to_remove) {
        if (m_display_lists.remove(id.value()))
            removed_resources.display_lists.set(id);
        m_display_list_requires_direct_replay.remove(id.value());
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
    m_display_lists.remove_all_matching([&](auto id, auto const&) {
        if (resource_set.display_lists.contains(DisplayListResourceId { id }))
            return false;
        removed_resources.display_lists.set(DisplayListResourceId { id });
        return true;
    });
    m_display_list_requires_direct_replay.remove_all_matching([&](auto id, auto const&) {
        return !resource_set.display_lists.contains(DisplayListResourceId { id });
    });
    m_has_resources_added_since_last_retain = false;
    return removed_resources;
}

void DisplayListResourceStorage::set_video_sink(VideoSinkResourceId id, RefPtr<Media::VideoSink> sink)
{
    m_has_resources_added_since_last_retain = true;
    m_video_sinks.ensure(id.value(), [] { return make<DisplayListStoredVideoSinkResource>(); })->sink = move(sink);
}

RefPtr<Media::VideoSink const> DisplayListResourceStorage::video_sink(VideoSinkResourceId id) const
{
    auto stored = m_video_sinks.find(id.value());
    if (stored == m_video_sinks.end())
        return nullptr;
    return stored->value->sink;
}

}
