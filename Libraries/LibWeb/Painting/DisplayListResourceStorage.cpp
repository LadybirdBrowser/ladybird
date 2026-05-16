/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filter.h>
#include <LibGfx/Font/Font.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::Painting {

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

ExternalContentResourceId DisplayListResourceStorage::add_external_content_source(
    NonnullRefPtr<ExternalContentSource const> source)
{
    auto id = source->id();
    m_external_content_sources.ensure(id, [&] { return move(source); });
    return { id };
}

VideoFrameResourceId DisplayListResourceStorage::add_video_frame_source(NonnullRefPtr<VideoFrameSource const> source)
{
    auto id = source->id();
    m_video_frame_sources.ensure(id, [&] { return move(source); });
    return { id };
}

DisplayListResourceId DisplayListResourceStorage::add_display_list(NonnullRefPtr<DisplayList const> display_list)
{
    auto id = display_list->id();
    m_display_lists.ensure(id, [&] { return move(display_list); });
    return { id };
}

static ReadonlyBytes inline_data(ReadonlyBytes payload, DisplayListDataSpan span)
{
    VERIFY(static_cast<size_t>(span.offset) + span.size <= payload.size());
    return payload.slice(span.offset, span.size);
}

void DisplayListResourceStorage::append_referenced_resources_from(
    DisplayListResourceStorage const& source,
    ReadonlyBytes command_bytes)
{
    auto referenced_resources = source.collect_referenced_resources(command_bytes);
    for (auto id : referenced_resources.fonts)
        add_font(source.font(id));
    for (auto id : referenced_resources.image_frames)
        add_image_frame(source.image_frame(id));
    for (auto id : referenced_resources.external_content_sources)
        add_external_content_source(source.external_content_source(id));
    for (auto id : referenced_resources.video_frame_sources)
        add_video_frame_source(source.video_frame_source(id));
    for (auto id : referenced_resources.display_lists)
        add_display_list(source.display_list(id));
}

void DisplayListResourceStorage::collect_referenced_resources(
    ReadonlyBytes command_bytes,
    DisplayListResourceSet& referenced_resources) const
{
    auto add_display_list_resource = [&](DisplayListResourceId id, Optional<ReadonlyBytes> command_bytes_to_collect) {
        if (referenced_resources.display_lists.set(id, AK::HashSetExistingEntryBehavior::Keep) != HashSetResult::InsertedNewEntry)
            return;
        auto const& nested_display_list = display_list(id);
        collect_referenced_resources(command_bytes_to_collect.value_or(nested_display_list.command_bytes()), referenced_resources);
    };

    DisplayList::for_each_command_header(command_bytes, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        visit_display_list_command(header.type, payload, [&](auto const& command) {
            if constexpr (requires { command.font_id; })
                referenced_resources.fonts.set(command.font_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { command.frame_id; })
                referenced_resources.image_frames.set(command.frame_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { external_content_source(command.source_id); })
                referenced_resources.external_content_sources.set(command.source_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { video_frame_source(command.source_id); })
                referenced_resources.video_frame_sources.set(command.source_id, AK::HashSetExistingEntryBehavior::Keep);
            if constexpr (requires { command.paint_style; command.paint_kind; }) {
                if (command.paint_kind == decltype(command.paint_kind)::PaintStyle
                    && command.paint_style.type == DisplayListPaintStyleType::Pattern)
                    add_display_list_resource(command.paint_style.pattern_tile_display_list_id, {});
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
                if constexpr (requires { command.command_bytes; })
                    add_display_list_resource(command.display_list_id, inline_data(payload, command.command_bytes));
                else
                    add_display_list_resource(command.display_list_id, {});
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

DisplayListResourceTransaction DisplayListResourceStorage::create_transaction(
    DisplayListResourceSet const& previous,
    DisplayListResourceSet const& current) const
{
    DisplayListResourceTransaction transaction;

    for (auto id : current.fonts) {
        if (!previous.fonts.contains(id))
            transaction.fonts.append(font(id));
    }
    for (auto id : current.image_frames) {
        if (!previous.image_frames.contains(id))
            transaction.image_frames.append(image_frame(id));
    }
    for (auto id : current.external_content_sources) {
        if (!previous.external_content_sources.contains(id))
            transaction.external_content_sources.append(external_content_source(id));
    }
    for (auto id : current.video_frame_sources) {
        if (!previous.video_frame_sources.contains(id))
            transaction.video_frame_sources.append(video_frame_source(id));
    }
    for (auto id : current.display_lists) {
        if (!previous.display_lists.contains(id))
            transaction.display_lists.append(display_list(id));
    }

    for (auto id : previous.fonts) {
        if (!current.fonts.contains(id))
            transaction.font_ids_to_remove.append(id);
    }
    for (auto id : previous.image_frames) {
        if (!current.image_frames.contains(id))
            transaction.image_frame_ids_to_remove.append(id);
    }
    for (auto id : previous.external_content_sources) {
        if (!current.external_content_sources.contains(id))
            transaction.external_content_source_ids_to_remove.append(id);
    }
    for (auto id : previous.video_frame_sources) {
        if (!current.video_frame_sources.contains(id))
            transaction.video_frame_source_ids_to_remove.append(id);
    }
    for (auto id : previous.display_lists) {
        if (!current.display_lists.contains(id))
            transaction.display_list_ids_to_remove.append(id);
    }

    return transaction;
}

void DisplayListResourceStorage::apply_transaction(DisplayListResourceTransaction&& transaction)
{
    for (auto const& font : transaction.fonts)
        add_font(*font);
    for (auto const& frame : transaction.image_frames)
        add_image_frame(frame);
    for (auto& source : transaction.external_content_sources)
        add_external_content_source(move(source));
    for (auto& source : transaction.video_frame_sources)
        add_video_frame_source(move(source));
    for (auto& display_list : transaction.display_lists)
        add_display_list(move(display_list));

    for (auto id : transaction.font_ids_to_remove)
        m_fonts.remove(id.value());
    for (auto id : transaction.image_frame_ids_to_remove)
        m_image_frames.remove(id.value());
    for (auto id : transaction.external_content_source_ids_to_remove)
        m_external_content_sources.remove(id.value());
    for (auto id : transaction.video_frame_source_ids_to_remove)
        m_video_frame_sources.remove(id.value());
    for (auto id : transaction.display_list_ids_to_remove)
        m_display_lists.remove(id.value());
}

void DisplayListResourceStorage::retain_only(DisplayListResourceSet const& resource_set)
{
    m_fonts.remove_all_matching([&](auto id, auto const&) { return !resource_set.fonts.contains(FontResourceId { id }); });
    m_image_frames.remove_all_matching([&](auto id, auto const&) { return !resource_set.image_frames.contains(ImageFrameResourceId { id }); });
    m_external_content_sources.remove_all_matching([&](auto id, auto const&) { return !resource_set.external_content_sources.contains(ExternalContentResourceId { id }); });
    m_video_frame_sources.remove_all_matching([&](auto id, auto const&) { return !resource_set.video_frame_sources.contains(VideoFrameResourceId { id }); });
    m_display_lists.remove_all_matching([&](auto id, auto const&) { return !resource_set.display_lists.contains(DisplayListResourceId { id }); });
}

}
