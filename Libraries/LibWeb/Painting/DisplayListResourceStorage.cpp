/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filter.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::Painting {

bool DisplayListResourceSet::is_empty() const
{
    return fonts.is_empty()
        && image_frames.is_empty()
        && video_frames.is_empty()
        && display_lists.is_empty();
}

void DisplayListResourceSet::include(DisplayListResourceSet const& other)
{
    for (auto id : other.fonts)
        fonts.set(id, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : other.image_frames)
        image_frames.set(id, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : other.video_frames)
        video_frames.set(id, AK::HashSetExistingEntryBehavior::Keep);
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

DisplayListResourceStorage::~DisplayListResourceStorage() = default;

FontResourceId DisplayListResourceStorage::add_font(Gfx::Font const& font)
{
    auto id = font.id();
    m_fonts.ensure(id, [&]() -> NonnullRefPtr<Gfx::Font const> { return font; });
    return { id };
}

ImageFrameResourceId DisplayListResourceStorage::add_image_frame(Gfx::DecodedImageFrame const& frame)
{
    auto id = frame.id();
    m_image_frames.ensure(id, [&] { return frame; });
    return { id };
}

VideoFrameResourceId DisplayListResourceStorage::add_video_frame(VideoFrameResourceId id, RefPtr<Media::VideoFrame const> frame)
{
    m_video_frames.set(id.value(), move(frame), AK::HashSetExistingEntryBehavior::Keep);
    return id;
}

DisplayListResourceId DisplayListResourceStorage::add_display_list(NonnullRefPtr<DisplayList const> display_list, AccumulatedVisualContextTree const& visual_context_tree)
{
    auto id = display_list->id();
    m_display_lists.ensure(id, [&] {
        return DisplayListResource { move(display_list), visual_context_tree };
    });
    return { id };
}

DisplayListResourceId DisplayListResourceStorage::add_display_list(DisplayListResource&& resource)
{
    auto id = resource.display_list->id();
    m_display_lists.set(id, move(resource), AK::HashSetExistingEntryBehavior::Keep);
    return { id };
}

void DisplayListResourceStorage::set_font(FontResourceId id, NonnullRefPtr<Gfx::Font const> font)
{
    m_fonts.set(id.value(), move(font));
}

void DisplayListResourceStorage::set_image_frame(ImageFrameResourceId id, Gfx::DecodedImageFrame frame)
{
    m_image_frames.set(id.value(), move(frame));
}

static ReadonlyBytes inline_data(ReadonlyBytes payload, DisplayListDataSpan span)
{
    VERIFY(static_cast<size_t>(span.offset) + span.size <= payload.size());
    return payload.slice(span.offset, span.size);
}

void DisplayListResourceStorage::collect_referenced_resources(
    ReadonlyBytes command_bytes,
    DisplayListResourceSet& referenced_resources) const
{
    auto add_display_list_resource = [&](DisplayListResourceId id) {
        if (referenced_resources.display_lists.set(id, AK::HashSetExistingEntryBehavior::Keep) != HashSetResult::InsertedNewEntry)
            return;
        auto const& nested_display_list = display_list(id);
        collect_referenced_resources(nested_display_list.command_bytes(), referenced_resources);
    };

    DisplayList::for_each_command_header(command_bytes, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        visit_display_list_command(header.type, payload, [&](auto const& command) {
            if constexpr (requires { command.font_id; })
                referenced_resources.fonts.set(command.font_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { command.frame_id; })
                referenced_resources.image_frames.set(command.frame_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { command.video_frame_id; })
                referenced_resources.video_frames.set(command.video_frame_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { command.paint_style; command.paint_kind; }) {
                if (command.paint_kind == decltype(command.paint_kind)::PaintStyle
                    && command.paint_style.type == DisplayListPaintStyleType::Pattern)
                    add_display_list_resource(command.paint_style.pattern_tile_display_list_id);
            }
            if constexpr (requires { command.backdrop_filter_data; }) {
                if (command.has_backdrop_filter) {
                    Gfx::deserialize_filter(inline_data(payload, command.backdrop_filter_data), [&](u64 image_id) {
                        referenced_resources.image_frames.set(ImageFrameResourceId { image_id }, AK::HashSetExistingEntryBehavior::Keep);
                        return image_frame(ImageFrameResourceId { image_id });
                    });
                }
            }
            if constexpr (requires { command.filter_data; }) {
                if (command.has_filter) {
                    Gfx::deserialize_filter(inline_data(payload, command.filter_data), [&](u64 image_id) {
                        referenced_resources.image_frames.set(ImageFrameResourceId { image_id }, AK::HashSetExistingEntryBehavior::Keep);
                        return image_frame(ImageFrameResourceId { image_id });
                    });
                }
            }
            if constexpr (requires { command.display_list_id; }) {
                add_display_list_resource(command.display_list_id);
            }
        });
    });
}

DisplayListResourceSet DisplayListResourceStorage::collect_referenced_resources(ReadonlyBytes command_bytes) const
{
    DisplayListResourceSet referenced_resources;
    collect_referenced_resources(command_bytes, referenced_resources);
    return referenced_resources;
}

DisplayListResourceSet DisplayListResourceStorage::collect_referenced_resources(DisplayList const& display_list) const
{
    return collect_referenced_resources(display_list.command_bytes());
}

template<typename ResourceId>
static void increment_cache_reference_counts(HashMap<u64, size_t>& counts, HashTable<ResourceId> const& ids)
{
    for (auto id : ids) {
        auto& count = counts.ensure(id.value(), [] { return 0; });
        ++count;
    }
}

template<typename ResourceId>
static void decrement_cache_reference_counts(HashMap<u64, size_t>& counts, HashTable<ResourceId> const& ids)
{
    for (auto id : ids) {
        auto it = counts.find(id.value());
        VERIFY(it != counts.end());
        VERIFY(it->value > 0);
        --it->value;
        if (it->value == 0)
            counts.remove(it);
    }
}

void DisplayListResourceStorage::acquire_cache_references(DisplayListResourceSet const& resource_set)
{
    increment_cache_reference_counts(m_font_cache_reference_counts, resource_set.fonts);
    increment_cache_reference_counts(m_image_frame_cache_reference_counts, resource_set.image_frames);
    increment_cache_reference_counts(m_video_frame_cache_reference_counts, resource_set.video_frames);
    increment_cache_reference_counts(m_display_list_cache_reference_counts, resource_set.display_lists);
}

void DisplayListResourceStorage::release_cache_references(DisplayListResourceSet const& resource_set)
{
    decrement_cache_reference_counts(m_font_cache_reference_counts, resource_set.fonts);
    decrement_cache_reference_counts(m_image_frame_cache_reference_counts, resource_set.image_frames);
    decrement_cache_reference_counts(m_video_frame_cache_reference_counts, resource_set.video_frames);
    decrement_cache_reference_counts(m_display_list_cache_reference_counts, resource_set.display_lists);
}

DisplayListResourceSet DisplayListResourceStorage::cache_referenced_resources() const
{
    DisplayListResourceSet resource_set;
    for (auto id : m_font_cache_reference_counts.keys())
        resource_set.fonts.set(FontResourceId { id });
    for (auto id : m_image_frame_cache_reference_counts.keys())
        resource_set.image_frames.set(ImageFrameResourceId { id });
    for (auto id : m_video_frame_cache_reference_counts.keys())
        resource_set.video_frames.set(VideoFrameResourceId { id });
    for (auto id : m_display_list_cache_reference_counts.keys())
        resource_set.display_lists.set(DisplayListResourceId { id });
    return resource_set;
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
    for (auto id : current.video_frames) {
        if (!previous.video_frames.contains(id))
            transaction.video_frames.append({ id, video_frame(id) });
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
    for (auto id : previous.video_frames) {
        if (!current.video_frames.contains(id))
            transaction.video_frame_ids_to_remove.append(id);
    }
    for (auto id : previous.display_lists) {
        if (!current.display_lists.contains(id))
            transaction.display_list_ids_to_remove.append(id);
    }
    return transaction;
}

void DisplayListResourceStorage::apply_transaction(DisplayListResourceTransaction&& transaction)
{
    for (auto& font : transaction.fonts)
        set_font(font.id, move(font.font));
    for (auto& frame : transaction.image_frames)
        set_image_frame(frame.id, move(frame.frame));
    for (auto& video_frame : transaction.video_frames)
        add_video_frame(video_frame.id, move(video_frame.frame));
    for (auto& display_list : transaction.display_lists)
        add_display_list(move(display_list));

    for (auto id : transaction.font_ids_to_remove)
        m_fonts.remove(id.value());
    for (auto id : transaction.image_frame_ids_to_remove)
        m_image_frames.remove(id.value());
    for (auto id : transaction.video_frame_ids_to_remove)
        m_video_frames.remove(id.value());
    for (auto id : transaction.display_list_ids_to_remove)
        m_display_lists.remove(id.value());
}

void DisplayListResourceStorage::retain_only(DisplayListResourceSet const& resource_set)
{
    m_fonts.remove_all_matching([&](auto id, auto const&) {
        return !resource_set.fonts.contains(FontResourceId { id })
            && !m_font_cache_reference_counts.contains(id);
    });
    m_image_frames.remove_all_matching([&](auto id, auto const&) {
        return !resource_set.image_frames.contains(ImageFrameResourceId { id })
            && !m_image_frame_cache_reference_counts.contains(id);
    });
    m_video_frames.remove_all_matching([&](auto id, auto const&) {
        return !resource_set.video_frames.contains(VideoFrameResourceId { id })
            && !m_video_frame_cache_reference_counts.contains(id);
    });
    m_display_lists.remove_all_matching([&](auto id, auto const&) {
        return !resource_set.display_lists.contains(DisplayListResourceId { id })
            && !m_display_list_cache_reference_counts.contains(id);
    });
}

void DisplayListResourceStorage::update_video_frame(VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    m_video_frames.set(frame_id.value(), move(frame));
}

void DisplayListResourceStorage::clear_video_frame(VideoFrameResourceId frame_id)
{
    if (m_video_frames.contains(frame_id.value()))
        m_video_frames.set(frame_id.value(), nullptr);
}

void DisplayListResourceStorage::update_compositor_surface(CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image)
{
    auto shared_image_buffer = Gfx::SharedImageBuffer::import_from_shared_image(move(shared_image));
    m_compositor_surfaces.set(surface_id.value(), Gfx::DecodedImageFrame { *shared_image_buffer.bitmap() });
}

void DisplayListResourceStorage::clear_compositor_surface(CompositorSurfaceId surface_id)
{
    m_compositor_surfaces.remove(surface_id.value());
}

}
