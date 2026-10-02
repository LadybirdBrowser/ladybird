/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/NavigablePresenter.h>

namespace Web::Compositor {

void NavigablePresenter::did_hand_display_list_to_compositor(NonnullRefPtr<Compositing::DisplayList> display_list, HTML::PaintConfig paint_config, Compositing::DisplayListResourceSet command_resources, Compositing::DisplayListResourceSet resources)
{
    m_compositor_display_list_visual_context_tree_structural_epoch = display_list->compatible_visual_context_tree_structural_epoch();
    m_resource_storage.retain_only(resources);
    m_compositor_display_list = move(display_list);
    m_compositor_display_list_command_resources = move(command_resources);
    m_compositor_display_list_resources = move(resources);
    m_compositor_display_list_paint_config = paint_config;
}

void NavigablePresenter::did_hand_visual_context_tree_to_compositor(Compositing::DisplayListResourceSet resources)
{
    m_resource_storage.retain_only(resources);
    m_compositor_display_list_resources = move(resources);
}

void NavigablePresenter::forget_compositor_display_list()
{
    m_compositor_display_list_paint_config.clear();
    m_compositor_display_list = nullptr;
    m_compositor_display_list_resources = {};
    m_compositor_display_list_command_resources = {};
}

}
