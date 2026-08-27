/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Debug.h>
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

DisplayList::DisplayList(u64 compatible_visual_context_tree_version, u64 id, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata> async_scrolling_metadata, HashMap<FrameNodeIndex, DisplayListResourceId>&& mask_display_lists)
    : m_compatible_visual_context_tree_version(compatible_visual_context_tree_version)
    , m_id(id)
    , m_command_bytes(move(command_bytes))
    , m_command_runs(move(command_runs))
    , m_surface_clear_color(surface_clear_color)
    , m_async_scrolling_metadata(move(async_scrolling_metadata))
    , m_mask_display_lists(move(mask_display_lists))
{
}

NonnullRefPtr<DisplayList> DisplayList::create_from_command_bytes(AccumulatedVisualContextTree const& visual_context_tree, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs)
{
    MUST(validate_display_list_command_runs(command_bytes, command_runs));
    auto display_list = create(visual_context_tree);
    display_list->m_command_bytes = move(command_bytes);
    display_list->m_command_runs = move(command_runs);
    return display_list;
}

Vector<DisplayListCommandRun> compute_display_list_command_runs(ReadonlyBytes command_bytes)
{
    Vector<DisplayListCommandRun> runs;
    u32 offset = 0;
    DisplayList::for_each_command_header(command_bytes, [&](DisplayListCommandHeader const& header, ReadonlyBytes) {
        auto record_size = static_cast<u32>(sizeof(DisplayListCommandHeader) + header.payload_size);
        if (runs.is_empty() || runs.last().context != header.context) {
            DisplayListCommandRun new_run {};
            new_run.offset = offset;
            new_run.context = header.context;
            runs.append(new_run);
        }
        auto& run = runs.last();
        run.size += record_size;
        offset += record_size;
        auto nesting_level_change = display_list_command_nesting_level_change(header.command_type);
        if (header.is_clip && run.nesting_delta == 0)
            run.has_unconfined_clip = true;
        run.nesting_delta += nesting_level_change;
        run.min_relative_nesting = min(run.min_relative_nesting, run.nesting_delta);
        if (display_list_command_is_compositor_metadata(header.command_type)) {
            run.has_compositor_metadata = true;
        } else if (nesting_level_change == 0 && !header.is_clip) {
            if (header.has_bounding_rect)
                run.ink_bounds.unite(header.bounding_rect);
            else
                run.has_unbounded_draw = true;
        }
        run.is_self_contained = run.nesting_delta == 0 && run.min_relative_nesting == 0 && !run.has_unconfined_clip;
    });
    return runs;
}

ErrorOr<void> validate_display_list_command_runs(ReadonlyBytes command_bytes, ReadonlySpan<DisplayListCommandRun> runs)
{
    size_t next_offset = 0;
    for (auto const& run : runs) {
        if (run.offset != next_offset || run.size == 0 || run.size % DisplayList::command_alignment != 0)
            return Error::from_string_literal("Display list command runs do not cover the command bytes");
        next_offset += run.size;
    }
    if (next_offset != command_bytes.size())
        return Error::from_string_literal("Display list command runs do not cover the command bytes");
    if constexpr (DISPLAY_LIST_RUNS_DEBUG) {
        if (runs != compute_display_list_command_runs(command_bytes).span())
            return Error::from_string_literal("Display list command runs disagree with the command bytes");
    }
    return {};
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
    ScrollStateSnapshot const& scroll_state_snapshot)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    execute_impl(display_list, scroll_state_snapshot);
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state)
{
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());

    VERIFY(m_surface);
    auto command_runs = display_list.command_runs();

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

        auto bounding_rect = header.has_bounding_rect
            ? Optional<Gfx::IntRect>(header.bounding_rect)
            : Optional<Gfx::IntRect> {};

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

    // A run enters its context once. Only a self-contained run with known ink bounds may be
    // skipped as a whole, and only such a run offers its bounds to the layer-frame cull; any
    // other run gets none, so an open Save or a clip that outlives the run is never dropped.
    // Skipping a run with nothing to draw before entering its context spares the frame pushes.
    auto execute_run = [&](DisplayListCommandRun const& run) {
        if (backface_culled[run.context.spatial.value()])
            return;
        Optional<Gfx::IntRect> skippable_ink_bounds;
        if (run.is_self_contained && !run.has_unbounded_draw)
            skippable_ink_bounds = run.ink_bounds;
        if (skippable_ink_bounds.has_value() && skippable_ink_bounds->is_empty())
            return;
        if (switch_to_context(run.context, skippable_ink_bounds) == SwitchResult::CulledByEffect)
            return;
        ensure_ctm_space(run.context.spatial);
        if (skippable_ink_bounds.has_value() && would_be_fully_clipped_by_painter(*skippable_ink_bounds))
            return;
        DisplayList::for_each_command_header(display_list.command_bytes_of_run(run), execute_command);
    };

    if (!tree_has_sorting_contexts) {
        for (auto const& run : command_runs)
            execute_run(run);
    } else {
        for (auto const& step : build_depth_sorted_replay_plan(command_runs, visual_context_tree, transform_palette, draw_space, backface_culled)) {
            step.visit(
                [&](ReadonlySpan<DisplayListCommandRun> runs) {
                    for (auto const& run : runs)
                        execute_run(run);
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
    // Trivially copyable records, so they travel as raw bytes like the command tape does.
    auto const& command_runs = display_list.m_command_runs;
    TRY(encoder.encode_size(command_runs.size()));
    if (!command_runs.is_empty())
        TRY(encoder.append(reinterpret_cast<u8 const*>(command_runs.data()), command_runs.size() * sizeof(Web::Painting::DisplayListCommandRun)));
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
    auto command_run_count = TRY(decoder.decode_size());
    Vector<Web::Painting::DisplayListCommandRun> command_runs;
    TRY(command_runs.try_resize(command_run_count));
    if (!command_runs.is_empty())
        TRY(decoder.decode_into(Bytes { reinterpret_cast<u8*>(command_runs.data()), command_runs.size() * sizeof(Web::Painting::DisplayListCommandRun) }));
    TRY(Web::Painting::validate_display_list_command_runs(command_bytes, command_runs));
    return adopt_ref(*new Web::Painting::DisplayList(compatible_visual_context_tree_version, id, move(command_bytes), move(command_runs), surface_clear_color, move(async_scrolling_metadata), move(mask_display_lists)));
}

}
