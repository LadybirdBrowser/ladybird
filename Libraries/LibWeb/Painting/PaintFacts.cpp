/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintFacts.h>

namespace Web::Painting {

static bool paints_form_control_from_facts(Layout::Node const& layout_node)
{
    return layout_node.kind() == Layout::RustFFI::NodeKind::CheckBox || layout_node.kind() == Layout::RustFFI::NodeKind::RadioButton;
}

static void push_form_control_paint_facts_onto(HTML::HTMLInputElement const& input, Layout::Node const& layout_node)
{
    Layout::RustFFI::FfiFormControlPaintFacts facts {
        .enabled = input.enabled(),
        .checked = input.checked(),
        .indeterminate = input.indeterminate(),
        .being_activated = input.is_being_activated(),
    };
    Layout::RustFFI::layout_arena_set_form_control_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
}

void push_form_control_paint_facts(HTML::HTMLInputElement& input)
{
    auto const* layout_node = input.unsafe_layout_node();
    if (!layout_node || !paints_form_control_from_facts(*layout_node))
        return;
    push_form_control_paint_facts_onto(input, *layout_node);
}

static void push_canvas_paint_facts_onto(HTML::HTMLCanvasElement const& canvas, Layout::Node const& layout_node)
{
    Layout::RustFFI::FfiCanvasPaintFacts facts {};
    if (auto content_size = canvas.canvas_surface_content_size(); content_size.has_value()) {
        facts.has_content = true;
        facts.content_width = content_size->width();
        facts.content_height = content_size->height();
        facts.canvas_id = canvas.canvas_id().value().value();
        facts.content_generation = canvas.content_generation();
    }
    bool changed = Layout::RustFFI::layout_arena_set_canvas_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
    if (changed && has_committed_box(layout_node))
        invalidate_paint_cache(layout_node);
}

void push_canvas_paint_facts(HTML::HTMLCanvasElement const& canvas)
{
    auto const* layout_node = canvas.unsafe_layout_node();
    if (!layout_node || layout_node->kind() != Layout::RustFFI::NodeKind::CanvasBox)
        return;
    push_canvas_paint_facts_onto(canvas, *layout_node);
}

static Optional<u64> composited_context_id_for_navigable_container(HTML::NavigableContainer const& navigable_container)
{
    auto content_navigable = navigable_container.content_navigable();
    if (!content_navigable)
        return {};
    auto const& local_navigable = as<HTML::LocalNavigable>(*content_navigable);
    if (local_navigable.has_been_destroyed())
        return {};
    auto context_id = navigable_container.document().page().client().compositor_context_id_for_remote_child_frame(content_navigable->id());
    if (!context_id.has_value() && local_navigable.has_compositor_context()) {
        auto const* hosted_document = navigable_container.content_document_without_origin_check();
        if (!hosted_document || !hosted_document->is_render_blocked())
            context_id = local_navigable.compositor_context().id();
    }
    if (!context_id.has_value())
        return {};
    return context_id->value();
}

void reconcile_navigable_container_paint_facts(DOM::Document const& document)
{
    for (auto const* navigable_container : HTML::NavigableContainer::all_instances()) {
        if (&navigable_container->document() != &document)
            continue;
        auto const* layout_node = navigable_container->layout_node();
        if (!layout_node || !is_navigable_container_viewport_paintable(*layout_node))
            continue;
        Layout::RustFFI::FfiNavigableContainerPaintFacts facts {};
        if (auto context_id = composited_context_id_for_navigable_container(*navigable_container); context_id.has_value()) {
            facts.has_composited_context = true;
            facts.composited_context_id = *context_id;
        }
        bool changed = Layout::RustFFI::layout_arena_set_navigable_container_paint_facts(layout_node->arena_handle(), Layout::Node::slot_id(layout_node), facts);
        if (changed)
            invalidate_paint_cache(*layout_node);
    }
}

void push_paint_facts_after_style_attach(Layout::NodeWithStyle& layout_node)
{
    if (paints_form_control_from_facts(layout_node))
        push_form_control_paint_facts_onto(as<HTML::HTMLInputElement>(*layout_node.dom_node()), layout_node);
    else if (layout_node.kind() == Layout::RustFFI::NodeKind::CanvasBox)
        push_canvas_paint_facts_onto(as<HTML::HTMLCanvasElement>(*layout_node.dom_node()), layout_node);
}

}
