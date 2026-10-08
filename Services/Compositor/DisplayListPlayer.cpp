/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <Compositor/DisplayListPlayer.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Path.h>

namespace Compositor {

using namespace Compositing;

void DisplayListPlayer::execute(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceStorage const& resource_storage,
    ScrollStateSnapshot const& scroll_state_snapshot,
    RefPtr<Gfx::PaintingSurface> surface,
    CanvasSurfaceRegistry const* canvas_surface_registry)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
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
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    ScrollStateSnapshot scroll_state_snapshot;
    execute_impl(display_list, scroll_state_snapshot);
}

void DisplayListPlayer::execute_command_bytes_into_surface(ReadonlyBytes command_bytes, Gfx::PaintingSurface& target_surface)
{
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    ScrollStateSnapshot scroll_state_snapshot;
    execute_command_bytes(command_bytes, scroll_state_snapshot);
}

void DisplayListPlayer::execute_nested_display_list(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    ScrollStateSnapshot const& scroll_state_snapshot)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    execute_impl(display_list, scroll_state_snapshot);
}

// Builds the callbacks the Rust replay drives a player through.
struct DisplayListPlayer::ReplayCallbacks {
    static Compositing::RustFFI::FfiDisplayListReplayCallbacks for_player(DisplayListPlayer& player)
    {
        return {
            .context = &player,
            .canvas_matrix = [](void* context) -> Gfx::FloatMatrix4x4 { return static_cast<DisplayListPlayer*>(context)->canvas_matrix(); },
            .set_matrix = [](void* context, Gfx::FloatMatrix4x4 const* matrix) { static_cast<DisplayListPlayer*>(context)->set_matrix(*matrix); },
            .would_be_fully_clipped_by_painter = [](void* context, Gfx::IntRect rect) -> bool {
                return static_cast<DisplayListPlayer*>(context)->would_be_fully_clipped_by_painter(rect);
            },
            .push_clip = [](void* context, ReplayClip const* clip) { static_cast<DisplayListPlayer*>(context)->push_clip(*clip); },
            .push_clip_path = [](void* context, void const* path, Gfx::WindingRule winding_rule) { static_cast<DisplayListPlayer*>(context)->push_clip_path(*static_cast<Gfx::Path const*>(path), winding_rule); },
            .push_layer = [](void* context, ReplayLayer const* layer) { static_cast<DisplayListPlayer*>(context)->push_layer(*layer); },
            .push_mask = [](void* context, ReplayMask const* mask) { static_cast<DisplayListPlayer*>(context)->push_mask(*mask); },
            .pop_mask = [](void* context, ReplayMask const* mask, EffectNodeIndex effect) { static_cast<DisplayListPlayer*>(context)->pop_mask(*mask, effect); },
            .pop = [](void* context) { static_cast<DisplayListPlayer*>(context)->pop(); },
            .push_device_space_plane_clip = [](void* context, Gfx::FloatVector3 const* vertices, size_t vertex_count) {
                Gfx::Path path;
                path.move_to({ vertices[0].x(), vertices[0].y() });
                for (size_t i = 1; i < vertex_count; ++i)
                    path.line_to({ vertices[i].x(), vertices[i].y() });
                path.close();
                static_cast<DisplayListPlayer*>(context)->push_device_space_plane_clip(path); },
            .push_transform = [](void* context, Gfx::AffineTransform const* transform) { static_cast<DisplayListPlayer*>(context)->push_transform(*transform); },
            .push_clip_path_bytes = [](void* context, u8 const* path_bytes, size_t path_bytes_size, Gfx::WindingRule winding_rule) { static_cast<DisplayListPlayer*>(context)->push_clip_path(Gfx::Path::from_serialized_bytes({ path_bytes, path_bytes_size }), winding_rule); },
            .play_command = [](void* context, DisplayListCommandType command_type, u8 const* command, u8 const* payload, size_t payload_size) { static_cast<DisplayListPlayer*>(context)->play_command_bytes(command_type, command, { payload, payload_size }); },
        };
    }
};

void DisplayListPlayer::play_command_bytes(DisplayListCommandType command_type, u8 const* command, ReadonlyBytes payload)
{
    TemporaryChange current_command_payload_change { m_current_command_payload, payload };
    switch (command_type) {
#define PLAY_DISPLAY_LIST_COMMAND(command_type)                                                  \
    case DisplayListCommandType::command_type:                                                   \
        play_command(read_display_list_object<command_type>({ command, sizeof(command_type) })); \
        break;
        ENUMERATE_DISPLAY_LIST_COMMANDS(PLAY_DISPLAY_LIST_COMMAND)
#undef PLAY_DISPLAY_LIST_COMMAND
    }
}

void DisplayListPlayer::execute_command_bytes(ReadonlyBytes command_bytes, ScrollStateSnapshot const& scroll_state)
{
    DisplayList::replay_records(command_bytes, scroll_state, ReplayCallbacks::for_player(*this));
}

void DisplayListPlayer::declare_mask_content(EffectNodeIndex effect, ReadonlyBytes content)
{
    m_declared_mask_contents.set(effect.value(), content);
}

Optional<ReadonlyBytes> DisplayListPlayer::declared_mask_content(EffectNodeIndex effect) const
{
    return m_declared_mask_contents.get(effect.value());
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state)
{
    TemporaryChange active_scroll_state_change { m_active_scroll_state, &scroll_state };
    TemporaryChange declared_mask_contents_change { m_declared_mask_contents, HashMap<u32, ReadonlyBytes> {} };
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    VERIFY(m_surface);

    auto callbacks = ReplayCallbacks::for_player(*this);
    display_list.replay(visual_context_tree, scroll_state, callbacks);
}

}
