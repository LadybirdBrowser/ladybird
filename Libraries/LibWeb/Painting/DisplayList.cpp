/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NumericLimits.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Path.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Painting/DepthSortedReplayPlan.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

static Atomic<u64> s_next_id { 1 };

DisplayList::DisplayList(u64 compatible_visual_context_tree_version)
    : m_compatible_visual_context_tree_version(compatible_visual_context_tree_version)
    , m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

DisplayList::DisplayList(u64 compatible_visual_context_tree_version, u64 id, ByteBuffer&& command_bytes, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata> async_scrolling_metadata, HashMap<FrameNodeIndex, DisplayListResourceId>&& mask_display_lists)
    : m_compatible_visual_context_tree_version(compatible_visual_context_tree_version)
    , m_id(id)
    , m_command_bytes(move(command_bytes))
    , m_surface_clear_color(surface_clear_color)
    , m_async_scrolling_metadata(move(async_scrolling_metadata))
    , m_mask_display_lists(move(mask_display_lists))
{
}

bool DisplayList::append_bytes(
    DisplayListCommandType type,
    ReadonlyBytes payload,
    ReadonlyBytes inline_data,
    AccumulatedVisualContextTree const& visual_context_tree,
    ContextRef context,
    Optional<Gfx::IntRect> bounding_rect,
    bool is_clip)
{
    VERIFY(visual_context_tree.version() == m_compatible_visual_context_tree_version);
    if (visual_context_tree.has_empty_effective_clip(context.frame))
        return false;
    VERIFY(m_command_bytes.size() % DisplayList::command_alignment == 0);
    VERIFY(payload.size() <= NumericLimits<u32>::max());
    VERIFY(inline_data.size() <= NumericLimits<u32>::max() - payload.size());
    auto payload_size = payload.size() + inline_data.size();
    auto record_size = sizeof(DisplayListCommandHeader) + payload_size;
    constexpr auto command_alignment = DisplayList::command_alignment;
    auto trailing_padding = align_up_to(record_size, command_alignment) - record_size;
    VERIFY(trailing_padding <= NumericLimits<u32>::max() - payload_size);
    DisplayListCommandHeader header {
        .command_type = type,
        .has_bounding_rect = bounding_rect.has_value(),
        .is_clip = is_clip,
        .payload_size = static_cast<u32>(payload_size + trailing_padding),
        .context = context,
        .bounding_rect = bounding_rect.value_or({}),
    };
    auto header_bytes = display_list_object_bytes(header);
    m_command_bytes.append(header_bytes.data(), header_bytes.size());
    m_command_bytes.append(payload.data(), payload.size());
    if (!inline_data.is_empty())
        m_command_bytes.append(inline_data.data(), inline_data.size());
    m_command_bytes.resize(m_command_bytes.size() + trailing_padding, ByteBuffer::ZeroFillNewElements::Yes);
    return true;
}

void DisplayListPlayer::execute(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceStorage const& resource_storage,
    ScrollStateSnapshot const& scroll_state_snapshot,
    RefPtr<Gfx::PaintingSurface> surface,
    CanvasSurfaceRegistry const* canvas_surface_registry)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    m_surface = surface;
    m_active_display_list = &display_list;
    m_active_visual_context_tree = &visual_context_tree;
    m_resource_storage = &resource_storage;
    m_canvas_surface_registry = canvas_surface_registry;
    execute_impl(display_list, scroll_state_snapshot);
    m_canvas_surface_registry = nullptr;
    m_resource_storage = nullptr;
    m_active_visual_context_tree = nullptr;
    m_active_display_list = nullptr;
    m_surface = nullptr;
}

void DisplayListPlayer::execute_display_list_into_surface(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree, Gfx::PaintingSurface& target_surface)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    ScrollStateSnapshot scroll_state_snapshot;
    execute_impl(display_list, scroll_state_snapshot);
}

void DisplayListPlayer::execute_nested_display_list(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    ScrollStateSnapshot const& scroll_state_snapshot,
    ReadonlyBytes command_bytes)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    execute_impl(display_list, scroll_state_snapshot, command_bytes);
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state)
{
    execute_impl(display_list, scroll_state, display_list.command_bytes());
}

void DisplayListPlayer::execute_impl(
    DisplayList const& display_list,
    ScrollStateSnapshot const& scroll_state,
    ReadonlyBytes commands)
{
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());

    VERIFY(m_surface);

    // Cumulative to-root matrices for every spatial node, resolved against the live scroll offsets
    // and folded onto the canvas matrix at replay entry, so any node's space can be entered
    // absolutely with a single set_matrix(). Spatial nodes therefore never touch the canvas save
    // stack; only frames do. A backface marker's entry carries the flattened matrix that feeds its
    // cull test and its descendants, while content recorded directly under the marker belongs to
    // its parent's plane, so draw_space redirects the marker to the parent's entry. The storage
    // lives on the player so steady-state replays reuse warm capacity; moving it out for the
    // duration of the replay keeps re-entrant nested replays from clobbering the outer palette.
    auto palette_storage = move(m_replay_palette_storage);
    auto& transform_palette = palette_storage.to_root_matrices;
    auto& draw_space = palette_storage.draw_spaces;
    auto const& spatial_nodes = visual_context_tree.spatial_nodes();
    transform_palette.clear_with_capacity();
    transform_palette.ensure_capacity(spatial_nodes.size());
    draw_space.clear_with_capacity();
    draw_space.ensure_capacity(spatial_nodes.size());
    auto& backface_culled = palette_storage.backface_culled;
    backface_culled.clear_with_capacity();
    backface_culled.ensure_capacity(spatial_nodes.size());
    auto const replay_base_matrix = canvas_matrix();
    bool tree_has_sorting_contexts = false;
    for (size_t i = 0; i < spatial_nodes.size(); ++i) {
        auto const& node = spatial_nodes[i];
        auto parent = node.parent.value();
        auto append_spatial = [&](Gfx::FloatMatrix4x4 const& local_matrix, bool flattens_inherited_transform = false) {
            auto const& parent_matrix = i == 0 ? replay_base_matrix : transform_palette[parent];
            transform_palette.unchecked_append((flattens_inherited_transform ? Gfx::flattened(parent_matrix) : parent_matrix) * local_matrix);
            draw_space.unchecked_append(SpatialNodeIndex { static_cast<u32>(i) });
            backface_culled.unchecked_append(i == 0 ? false : backface_culled[parent]);
        };
        auto append_spatial_translation = [&](Gfx::IntPoint offset) {
            // Whole device pixels, so scrolled content never lands on subpixel positions.
            append_spatial(Gfx::translation_matrix(Vector3<float>(static_cast<float>(offset.x()), static_cast<float>(offset.y()), 0)));
        };
        node.data.visit(
            [&](TransformData const& transform) {
                tree_has_sorting_contexts |= transform.sorting_context_root_index.has_value();
                append_spatial(transform.matrix_including_origin(), transform.flattens_inherited_transform);
            },
            [&](PerspectiveData const& perspective) {
                append_spatial(perspective.matrix, perspective.flattens_inherited_transform);
            },
            [&](BackfaceVisibilityData const& backface) {
                auto const& parent_matrix = transform_palette[parent];
                transform_palette.unchecked_append(backface.flattens_inherited_transform ? Gfx::flattened(parent_matrix) : parent_matrix);
                draw_space.unchecked_append(draw_space[parent]);
                bool culled = backface_culled[parent];
                if (!culled) {
                    auto const& plane_root_matrix = transform_palette[backface.plane_root_index.value()];
                    culled = should_cull_back_face(transform_palette.last(), plane_root_matrix);
                }
                backface_culled.unchecked_append(culled);
            },
            [&](ScrollData const&) {
                append_spatial_translation(scroll_state.device_offset_for_index(SpatialNodeIndex { static_cast<u32>(i) }).to_type<int>());
            },
            [&](StickyData const&) {
                append_spatial_translation(scroll_state.device_offset_for_index(SpatialNodeIndex { static_cast<u32>(i) }).to_type<int>());
            },
            [&](AnchorScrollShift const& shift) {
                append_spatial_translation(shift.masked_offset(scroll_state).to_type<int>());
            });
    }

    // The palette entry the canvas matrix currently equals, if known; every Restore resets the
    // matrix to its save point, so unwinding applied frames invalidates it. Recorded streams
    // contain no matrix-mutating commands, so playing commands never invalidates the cache.
    Optional<SpatialNodeIndex> current_ctm_space;
    auto ensure_ctm_space = [&](SpatialNodeIndex spatial) {
        auto space = draw_space[spatial.value()];
        if (current_ctm_space == space)
            return;
        set_matrix(transform_palette[space.value()]);
        current_ctm_space = space;
    };

    // Each applied/target frame pushes and pops the canvas state according to its node kind;
    // a context without a frame targets an empty frame list and takes its coordinates from the palette.
    Vector<FrameNodeIndex, 16> applied_frames;
    Vector<FrameNodeIndex, 16> target_frames;
    Optional<ContextRef> applied_context;

    auto build_target_frames = [&](FrameNodeIndex target_frame) {
        target_frames.clear_with_capacity();
        for (auto frame = target_frame; frame != NO_FRAME_NODE; frame = visual_context_tree.frame_node_at(frame).parent)
            target_frames.append(frame);
        target_frames.reverse();
    };

    size_t applied_mask_frame_count = 0;
    auto restore_to_length = [&](size_t length) {
        applied_context = {};
        while (applied_frames.size() > length) {
            auto frame_index = applied_frames.take_last();
            auto const& frame_node = visual_context_tree.frame_node_at(frame_index);
            auto const* mask = applied_mask_frame_count > 0 ? frame_node.data.get_pointer<MaskData>() : nullptr;
            if (mask) {
                --applied_mask_frame_count;
                ensure_ctm_space(frame_node.spatial);
                play_command(ApplyEffects {
                                 .opacity = 1.0f,
                                 .compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::DestinationIn,
                                 .has_filter = false,
                                 .filter_data = {},
                                 .has_mask_kind = mask->kind == Gfx::MaskKind::Luminance,
                                 .mask_kind = mask->kind,
                             },
                    nullptr);
                if (auto display_list_id = display_list.mask_display_list_id(frame_index);
                    display_list_id.has_value() && resource_storage().has_display_list(*display_list_id)) {
                    auto mask_rect = mask->rect.to_type<int>();
                    play_command(PaintNestedDisplayList {
                        .display_list_id = *display_list_id,
                        .rect = mask_rect.to_type<float>(),
                        .list_size = mask_rect.size(),
                    });
                }
                play_command(Restore {}); // DstIn layer
                play_command(Restore {}); // content layer
                play_command(Restore {}); // clip save
            } else {
                play_command(Restore {});
            }
            current_ctm_space = {};
        }
    };

    // OPTIMIZATION: When walking down to layer-pushing frames (effects and masks), check culling before pushing
    //               each one. Effects don't affect clip state and a mask push only narrows it, so testing against
    //               the pre-push clip is conservative and valid. This avoids expensive saveLayer/restore cycles
    //               for off-screen elements.
    enum class SwitchResult : u8 {
        Switched,
        CulledByEffect,
    };
    auto switch_to_context = [&](ContextRef target, Optional<Gfx::IntRect> bounding_rect = {}) -> SwitchResult {
        if (applied_context == target)
            return SwitchResult::Switched;

        build_target_frames(target.frame);

        auto common_prefix_length = applied_frames.span().matching_prefix_length(target_frames);

        restore_to_length(common_prefix_length);

        for (size_t i = common_prefix_length; i < target_frames.size(); ++i) {
            auto frame_index = target_frames[i];
            auto const& frame_node = visual_context_tree.frame_node_at(frame_index);
            auto const* effects = frame_node.data.get_pointer<EffectsData>();
            bool pushes_layer = effects || frame_node.data.has<MaskData>();
            if (pushes_layer && bounding_rect.has_value()) {
                bool culled_by_layer_frame = bounding_rect->is_empty();
                if (!culled_by_layer_frame) {
                    ensure_ctm_space(target.spatial);
                    culled_by_layer_frame = would_be_fully_clipped_by_painter(*bounding_rect);
                }
                if (culled_by_layer_frame) {
                    restore_to_length(common_prefix_length);
                    // The canvas is unwound to the shared prefix; clearing the applied context
                    // keeps the fast path from reusing the pre-cull context while the frame
                    // vector still enables prefix reuse on the next switch.
                    return SwitchResult::CulledByEffect;
                }
            }
            if (effects) {
                ensure_ctm_space(frame_node.spatial);
                play_command(ApplyEffects {
                                 .opacity = effects->opacity,
                                 .compositing_and_blending_operator = effects->blend_mode,
                                 .has_filter = effects->gfx_filter.has_value(),
                                 .filter_data = {},
                                 .has_mask_kind = false,
                                 .mask_kind = {},
                             },
                    effects->gfx_filter.has_value() ? &effects->gfx_filter.value() : nullptr);
            } else {
                play_command(Save {});
                ensure_ctm_space(frame_node.spatial);
                frame_node.data.visit(
                    [&](ClipData const& clip) {
                        if (clip.corner_radii.has_any_radius()) {
                            play_command(AddRoundedRectClip {
                                .corner_radii = clip.corner_radii,
                                .border_rect = clip.rect.to_type<int>(),
                                .corner_clip = Gfx::CornerClip::Outside,
                            });
                        } else {
                            play_command(AddClipRect { .rect = clip.rect.to_type<int>().to_type<float>() });
                        }
                    },
                    [&](ClipPathData const& clip_path) {
                        add_clip_path(clip_path.path, clip_path.fill_rule, true);
                    },
                    [&](MaskData const& mask) {
                        play_command(AddClipRect { .rect = mask.rect.to_type<int>().to_type<float>() });
                        play_command(SaveLayer {});
                        ++applied_mask_frame_count;
                    },
                    [&](EffectsData const&) { VERIFY_NOT_REACHED(); });
            }
            applied_frames.append(frame_index);
        }

        applied_context = target;
        return SwitchResult::Switched;
    };

    auto execute_command = [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (display_list_command_is_compositor_metadata(header.command_type))
            return;

        auto context = header.context;
        if (backface_culled[context.spatial.value()])
            return;

        auto bounding_rect = header.has_bounding_rect
            ? Optional<Gfx::IntRect>(header.bounding_rect)
            : Optional<Gfx::IntRect> {};

        if (switch_to_context(context, bounding_rect) == SwitchResult::CulledByEffect)
            return;

        ensure_ctm_space(context.spatial);

        if (bounding_rect.has_value() && (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect))) {
            // Any clip that's located outside of the visible region is equivalent to a simple clip-rect,
            // so replace it with one to avoid doing unnecessary work.
            if (header.is_clip) {
                if (header.command_type == DisplayListCommandType::AddClipRect)
                    play_command(read_display_list_command_payload<AddClipRect>(payload));
                else
                    play_command(AddClipRect { bounding_rect.release_value().to_type<float>() });
            }
            return;
        }

        TemporaryChange current_command_payload_change { m_current_command_payload, payload };
        auto dispatch_command = [&]<DisplayListCommand Command>(auto&& callback) {
            auto command = read_display_list_command_payload<Command>(payload);
            if constexpr (IsSame<Command, PaintScrollBar>) {
                auto device_offset = scroll_state.device_offset_for_index(command.scroll_node_index);
                if (command.vertical)
                    command.thumb_rect.translate_by(0, static_cast<int>(-device_offset.y() * command.scroll_size));
                else
                    command.thumb_rect.translate_by(static_cast<int>(-device_offset.x() * command.scroll_size), 0);
            }
            callback(command);
        };

        switch (header.command_type) {
#define DISPATCH_DISPLAY_LIST_COMMAND(command_type, player_method)                    \
    case DisplayListCommandType::command_type:                                        \
        dispatch_command.template operator()<command_type>([&](auto const& command) { \
            play_command(command);                                                    \
        });                                                                           \
        break;
            ENUMERATE_DISPLAY_LIST_COMMANDS(DISPATCH_DISPLAY_LIST_COMMAND)
#undef DISPATCH_DISPLAY_LIST_COMMAND
        }
    };

    if (!tree_has_sorting_contexts) {
        DisplayList::for_each_command_header(commands, execute_command);
    } else {
        for (auto const& step : build_depth_sorted_replay_plan(commands, visual_context_tree, transform_palette, draw_space, backface_culled)) {
            step.visit(
                [&](DisplayListCommandRange const& range) {
                    DisplayList::for_each_command_header(commands.slice(range.offset, range.size), execute_command);
                },
                [&](PushPlaneClip const& clip) {
                    restore_to_length(0);
                    play_command(Save {});
                    set_matrix(Gfx::FloatMatrix4x4::identity());
                    current_ctm_space = {};
                    Gfx::Path path;
                    path.move_to({ clip.vertices[0].x(), clip.vertices[0].y() });
                    for (size_t i = 1; i < clip.vertices.size(); ++i)
                        path.line_to({ clip.vertices[i].x(), clip.vertices[i].y() });
                    path.close();
                    add_clip_path(path, Gfx::WindingRule::Nonzero, false);
                },
                [&](PopPlaneClip const&) {
                    restore_to_length(0);
                    play_command(Restore {});
                    current_ctm_space = {};
                });
        }
    }

    restore_to_length(0);
    // Node spaces were entered by setting the canvas matrix absolutely, outside any save, so the
    // matrix the replay entered with must be handed back explicitly.
    set_matrix(replay_base_matrix);

    m_replay_palette_storage = move(palette_storage);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayList::AsyncScrollingMetadata const& metadata)
{
    TRY(encoder.encode(metadata.viewport_rect));
    TRY(encoder.encode(metadata.wheel_event_listener_state_generation));
    TRY(encoder.encode(metadata.has_blocking_wheel_event_listeners));
    TRY(encoder.encode(metadata.has_blocking_wheel_event_region_covering_viewport));
    return {};
}

template<>
ErrorOr<Web::Painting::DisplayList::AsyncScrollingMetadata> decode(Decoder& decoder)
{
    return Web::Painting::DisplayList::AsyncScrollingMetadata {
        .viewport_rect = TRY(decoder.decode<Gfx::IntRect>()),
        .wheel_event_listener_state_generation = TRY(decoder.decode<u64>()),
        .has_blocking_wheel_event_listeners = TRY(decoder.decode<bool>()),
        .has_blocking_wheel_event_region_covering_viewport = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayList const& display_list)
{
    TRY(encoder.encode(display_list.m_id));
    TRY(encoder.encode(display_list.m_command_bytes));
    TRY(encoder.encode(display_list.m_compatible_visual_context_tree_version));
    TRY(encoder.encode(display_list.m_surface_clear_color));
    TRY(encoder.encode(display_list.m_async_scrolling_metadata));
    TRY(encoder.encode(display_list.m_mask_display_lists));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, NonnullRefPtr<Web::Painting::DisplayList> const& display_list)
{
    return encoder.encode(*display_list);
}

template<>
ErrorOr<NonnullRefPtr<Web::Painting::DisplayList>> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u64>());
    auto command_bytes = TRY(decoder.decode<ByteBuffer>());
    auto compatible_visual_context_tree_version = TRY(decoder.decode<u64>());
    auto surface_clear_color = TRY(decoder.decode<Optional<Gfx::Color>>());
    auto async_scrolling_metadata = TRY(decoder.decode<Optional<Web::Painting::DisplayList::AsyncScrollingMetadata>>());
    auto mask_display_lists = TRY(decoder.decode<HashMap<Web::Painting::FrameNodeIndex, Web::Painting::DisplayListResourceId>>());
    return adopt_ref(*new Web::Painting::DisplayList(compatible_visual_context_tree_version, id, move(command_bytes), surface_clear_color, move(async_scrolling_metadata), move(mask_display_lists)));
}

}
