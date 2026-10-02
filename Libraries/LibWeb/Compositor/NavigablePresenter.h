/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/PaintConfig.h>

namespace Web::Compositor {

// What a navigable presents to its compositor context from: the resource storage its recordings add to, and the
// display list the compositor context holds with the resources it holds for it. The presenter holds no GC pointer.
class WEB_API NavigablePresenter {
    AK_MAKE_NONCOPYABLE(NavigablePresenter);
    AK_MAKE_NONMOVABLE(NavigablePresenter);

public:
    NavigablePresenter() = default;

    Compositing::DisplayListResourceStorage& display_list_resource_storage() { return m_resource_storage; }
    Compositing::DisplayListResourceStorage const& display_list_resource_storage() const { return m_resource_storage; }

    // The display list the compositor context holds, and the paint config it was recorded with.
    RefPtr<Compositing::DisplayList> const& compositor_display_list() const { return m_compositor_display_list; }
    Optional<HTML::PaintConfig> const& compositor_display_list_paint_config() const { return m_compositor_display_list_paint_config; }
    void set_compositor_display_list_paint_config(HTML::PaintConfig paint_config) { m_compositor_display_list_paint_config = paint_config; }
    u64 compositor_display_list_visual_context_tree_structural_epoch() const { return m_compositor_display_list_visual_context_tree_structural_epoch; }

    // The resources the compositor context holds for its display list and visual context tree, and those the display
    // list's commands reference.
    Compositing::DisplayListResourceSet const& compositor_display_list_resources() const { return m_compositor_display_list_resources; }
    Compositing::DisplayListResourceSet const& compositor_display_list_command_resources() const { return m_compositor_display_list_command_resources; }

    // The compositor context now holds `display_list`, recorded with `paint_config`.
    void did_hand_display_list_to_compositor(NonnullRefPtr<Compositing::DisplayList>, HTML::PaintConfig, Compositing::DisplayListResourceSet command_resources, Compositing::DisplayListResourceSet resources);
    // The compositor context now holds a new visual context tree for its display list.
    void did_hand_visual_context_tree_to_compositor(Compositing::DisplayListResourceSet resources);
    // Forgets what the compositor context holds: a new compositor process holds nothing.
    void forget_compositor_display_list();

private:
    Compositing::DisplayListResourceStorage m_resource_storage;
    Optional<HTML::PaintConfig> m_compositor_display_list_paint_config;
    RefPtr<Compositing::DisplayList> m_compositor_display_list;
    u64 m_compositor_display_list_visual_context_tree_structural_epoch { 0 };
    Compositing::DisplayListResourceSet m_compositor_display_list_resources;
    Compositing::DisplayListResourceSet m_compositor_display_list_command_resources;
};

}
