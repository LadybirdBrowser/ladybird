/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>

namespace Web::DOM {

static Layout::RustFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

// The document-side steps of the layout update, which the Rust loop drives through this table.
Layout::RustFFI::FfiLayoutUpdateHostCallbacks Document::layout_update_host_callbacks()
{
    return {
        .context = this,
        .connected_element_count = [](void* context) -> u32 { return static_cast<Document*>(context)->style_computer().style_engine().connected_element_count(); },
        .update_style = [](void* context) { static_cast<Document*>(context)->update_style(); },
        .process_pending_list_item_renumbers = [](void* context) { static_cast<Document*>(context)->process_pending_list_item_renumbers(); },
        .process_pending_top_layer_layout_changes = [](void* context) { static_cast<Document*>(context)->process_pending_top_layer_layout_changes(); },
        .document_facts = [](void* context) -> Layout::RustFFI::FfiLayoutUpdateDocumentFacts {
            auto& document = *static_cast<Document*>(context);
            auto navigable = document.navigable();
            bool document_is_active = navigable && navigable->active_document().ptr() == &document;
            auto viewport_rect = document_is_active ? navigable->viewport_rect() : CSSPixelRect {};
            return {
                .document_is_active = document_is_active,
                .layout_root = Layout::Node::slot_id(document.m_layout_root),
                .document_needs_layout_tree_build = document.needs_layout_tree_update() || document.child_needs_layout_tree_update(),
                .needs_full_layout_tree_update = document.needs_full_layout_tree_update(),
                .container_query_evaluation_is_pending = !document.m_query_containers_needing_container_query_evaluation_after_layout.is_empty(),
                .top_layer_work_pending = document.m_top_layer_needs_layout_zone_rebuild || !document.m_elements_with_pending_top_layer_membership_change.is_empty(),
                .should_collect_devtools_layout_data = document.page().client().has_active_devtools_client(),
                .document_in_quirks_mode = document.in_quirks_mode(),
                .viewport_inline_size_raw = viewport_rect.width().raw_value(),
                .viewport_block_size_raw = viewport_rect.height().raw_value(),
            }; },
        .needs_style_update_after_layout = [](void* context) -> bool { return static_cast<Document*>(context)->needs_style_update_after_layout(); },
        .prepare_for_rendering = [](void* context) { static_cast<Document*>(context)->prepare_for_rendering(); },
        .build_layout_tree = [](void* context) -> Layout::RustFFI::FfiLayoutTreeBuildOutcome {
            auto& document = *static_cast<Document*>(context);
            auto outcome = Layout::build_layout_tree(document);
            document.set_layout_root(outcome.viewport);
            return outcome; },
        .clear_needs_full_layout_tree_update = [](void* context) { static_cast<Document*>(context)->set_needs_full_layout_tree_update(false); },
        .reconcile_stale_list_item_counters_after_tree_build = [](void* context) -> bool { return static_cast<Document*>(context)->reconcile_stale_list_item_counters_after_tree_build(); },
        .after_layout_commit = [](void* context, bool layout_tree_changed) { static_cast<Document*>(context)->after_layout_commit(layout_tree_changed ? LayoutTreeChanged::Yes : LayoutTreeChanged::No); },
        .note_full_layout_performed = [](void* context) { static_cast<Document*>(context)->style_invalidation_counters().relayouts_performed++; },
        .evaluate_pending_container_queries = [](void* context) {
            auto& document = *static_cast<Document*>(context);
            if (document.m_query_containers_needing_container_query_evaluation_after_layout.is_empty())
                return;
            auto query_containers = exchange(document.m_query_containers_needing_container_query_evaluation_after_layout, {});
            for (auto& query_container : query_containers) {
                if (!query_container->is_connected())
                    continue;

                CSS::Invalidation::invalidate_descendant_styles_depending_on_size_container_query(query_container);
            } },
        .record_stabilization_bound_failure = [](void* context) { ++static_cast<Document*>(context)->m_style_invalidation_counters.style_stabilization_bound_failures; },
    };
}

void Document::update_layout(UpdateLayoutReason reason)
{
    update_layout(reason, ThrottledAnimationSamplingScope::Document);
}

void Document::update_layout(UpdateLayoutReason reason, ThrottledAnimationSamplingScope animation_sampling_scope)
{
    auto navigable = this->navigable();
    if (!navigable || navigable->active_document().ptr() != this)
        return;

    // Internal layout dependencies do not observe compositor animation values.
    if (reason != UpdateLayoutReason::HTMLEventLoopRenderingUpdate
        && reason != UpdateLayoutReason::ChildDocumentStyleUpdate
        && animation_sampling_scope == ThrottledAnimationSamplingScope::Document)
        flush_throttled_animation_style_update();

    auto& arena = layout_node_arena();
    VERIFY(!m_is_running_update_layout);
    m_is_running_update_layout = true;
    Layout::RustFFI::layout_arena_begin_update_layout(arena.handle());
    ScopeGuard guard = [&] {
        Layout::RustFFI::layout_arena_end_update_layout(arena.handle());
        m_is_running_update_layout = false;

        if (m_needs_scroll_container_resnap) {
            if (auto navigable = this->navigable(); navigable && navigable->active_document().ptr() == this)
                navigable->re_snap_scroll_containers_after_layout_change();
        }

        page().client().flush_pending_dom_mutations();
    };

    begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        end_style_stabilization_epoch();
    };

    // Keep shared style records alive across both style and layout, so temporary views
    // during layout tree construction and layout do not need individual record pins.
    style_computer().begin_style_record_view_epoch();
    ScopeGuard end_style_record_view_epoch = [&] {
        style_computer().end_style_record_view_epoch();
    };

    Layout::RustFFI::FfiLayoutUpdateInputs inputs {
        .reason_is_inspect_devtools_layout_data = reason == UpdateLayoutReason::InspectDevToolsLayoutData,
        .is_template_contents_document = m_created_for_appropriate_template_contents,
        .reason_name = ffi_utf16_view(to_string(reason)),
    };
    Layout::RustFFI::layout_arena_update_layout(arena.handle(), &inputs);
}

}
