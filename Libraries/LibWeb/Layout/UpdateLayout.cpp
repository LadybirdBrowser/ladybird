/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGfx/FontCascadeList.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DocumentPaintState.h>

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
        .connected_element_count = [](void* context, Layout::BegunRead const* read) -> u32 { return CSS::StyleEngineFFI::style_engine_connected_element_count(static_cast<Document*>(context)->style_computer().style_engine().host(), read); },
        .update_style = [](void* context, Layout::BegunRead const*) { static_cast<Document*>(context)->update_style(); },
        .process_pending_list_item_renumbers = [](void* context, Layout::BegunRead const*) { static_cast<Document*>(context)->process_pending_list_item_renumbers(); },
        .process_pending_top_layer_layout_changes = [](void* context, Layout::BegunRead const* read) { static_cast<Document*>(context)->process_pending_top_layer_layout_changes(*read); },
        .document_facts = [](void* context, Layout::BegunRead const*) -> Layout::RustFFI::FfiLayoutUpdateDocumentFacts {
            auto& document = *static_cast<Document*>(context);
            auto navigable = document.navigable();
            bool document_is_active = navigable && navigable->active_document().ptr() == &document;
            auto viewport_rect = document_is_active ? navigable->viewport_rect() : CSSPixelRect {};
            return {
                .document_is_active = document_is_active,
                .document_needs_layout_tree_build = document.needs_layout_tree_update() || document.child_needs_layout_tree_update(),
                .top_layer_work_pending = document.m_top_layer_needs_layout_zone_rebuild || !document.m_elements_with_pending_top_layer_membership_change.is_empty(),
                .should_collect_devtools_layout_data = document.page().client().has_active_devtools_client(),
                .document_in_quirks_mode = document.in_quirks_mode(),
                .viewport_inline_size_raw = viewport_rect.width().raw_value(),
                .viewport_block_size_raw = viewport_rect.height().raw_value(),
                .document_style_node = document.style_node_id().value(),
                .has_stale_list_item_counters = !document.m_list_owners_with_stale_item_counters.is_empty(),
            }; },
        .container_query_evaluation_is_pending = [](void* context, Layout::BegunRead const* read) -> bool { return static_cast<Document*>(context)->has_size_containers_needing_evaluation_after_layout(*read); },
        .needs_style_update_after_layout = [](void* context, Layout::BegunRead const* read) -> bool { return static_cast<Document*>(context)->needs_style_update_after_layout(*read); },
        .prepare_for_rendering = [](void* context, Layout::BegunRead const* read) { static_cast<Document*>(context)->prepare_for_rendering(*read); },
        .prepare_layout_tree_build = [](void* context, Layout::BegunRead const* read, bool may_create_viewport) -> u64 {
            auto& document = *static_cast<Document*>(context);
            document.m_needs_throttled_animation_style_update_check = true;
            // The viewport's style is the document's, which the style computer makes rather than publishes, so a build
            // that may build the viewport is handed it before it starts.
            CSS::StyleRecordID document_style_record;
            if (may_create_viewport) {
                auto& style_computer = document.style_computer();
                document_style_record = style_computer.intern_anonymous_layout_style(*read, *style_computer.create_document_style());
            }
            // The viewport's row holds what the navigable has scrolled the viewport to, which the navigable publishes as
            // it scrolls. A new document has not heard from it yet.
            if (auto navigable = document.navigable())
                Layout::RustFFI::render_state_set_viewport_scroll_offset(document.layout_node_arena().host(), navigable->viewport_scroll_offset());
            return document_style_record.value(); },
        // The build records the root it placed in the arena itself, so what is left for the document is to retire the
        // tree that was replaced and give the new one a paint state.
        .finish_layout_tree_build = [](void* context, Layout::BegunRead const* read, Compositing::RustFFI::NodeSlotId replaced_root, Compositing::RustFFI::NodeSlotId viewport) {
            auto& document = *static_cast<Document*>(context);
            auto& arena = document.layout_node_arena();
            VERIFY(is<Layout::Viewport>(arena.node_if_live(*read, viewport)));
            if (replaced_root.index == viewport.index)
                return;
            if (auto* replaced_layout_root = arena.node_if_live(*read, replaced_root)) {
                replaced_layout_root->prepare_subtree_for_detach_from_layout_tree();
                arena.free_subtree(*read, replaced_root);
            }
            document.m_paint_state = make<Painting::DocumentPaintState>(arena); },
        .reconcile_stale_list_item_counters_after_tree_build = [](void* context, Layout::BegunRead const* read) -> bool { return static_cast<Document*>(context)->reconcile_stale_list_item_counters_after_tree_build(*read); },
        .after_layout_commit = [](void* context, Layout::BegunRead const* read, bool layout_tree_changed) { static_cast<Document*>(context)->after_layout_commit(*read, layout_tree_changed ? LayoutTreeChanged::Yes : LayoutTreeChanged::No); },
        .note_full_layout_performed = [](void* context) { static_cast<Document*>(context)->style_invalidation_counters().relayouts_performed++; },
        .evaluate_pending_container_queries = [](void* context, Layout::BegunRead const* read) { static_cast<Document*>(context)->style_computer().style_engine().evaluate_size_containers_needing_evaluation_after_layout(*read); },
        .record_stabilization_bound_failure = [](void* context) { ++static_cast<Document*>(context)->m_style_invalidation_counters.style_stabilization_bound_failures; },
        .attach_style_resources = [](void* context, Layout::BegunRead const* read, Compositing::RustFFI::NodeSlotId slot, bool owns_content_replacement_image, Layout::RustFFI::FfiStyleImageFacts images) {
            auto& document = *static_cast<Document*>(context);
            if (Layout::attach_owed_style_resources(*read, document, slot, owns_content_replacement_image, images))
                document.m_owed_image_provider_arrived_with_image = true; },
        .attach_generated_image = [](void* context, Layout::BegunRead const* read, Compositing::RustFFI::NodeSlotId slot, u32 element_style_node, Layout::RustFFI::FfiPseudoElement pseudo_element, Layout::RustFFI::FfiGeneratedImage image, Layout::RustFFI::FfiStyleImageFacts images) {
            auto& document = *static_cast<Document*>(context);
            if (Layout::attach_owed_generated_image(*read, document, slot, element_style_node, pseudo_element, image, images))
                document.m_owed_image_provider_arrived_with_image = true; },
    };
}

void Document::update_layout(UpdateLayoutReason reason)
{
    update_layout(reason, ThrottledAnimationSamplingScope::Document);
}

}

namespace Web::DOM {

void Document::update_layout(UpdateLayoutReason reason, ThrottledAnimationSamplingScope animation_sampling_scope)
{
    // The update's waits for the render state are one read, which takes a frame in flight in.
    Layout::ForcedReadScope read { style_computer().style_engine().render_document() };
    drain_flown_style_transaction(read);

    // An image box that owns its image's provider is handed it once the layout update that built the box is over, and
    // the update lays it out without an image. If the image was already there, the box lays out again with it before
    // the read goes on. Likewise, a web face a layout reached is requested once the update is over, and the update
    // runs again in case the face is already there. Only an update that builds another such box or reaches a face no
    // update reached before can leave one behind again, so this settles.
    auto update_style_and_layout = [&] {
        update_style_and_layout_once(read, reason, animation_sampling_scope);
        while (exchange(m_owed_image_provider_arrived_with_image, false) || exchange(m_requested_wanted_font_faces, false))
            update_style_and_layout_once(read, reason, animation_sampling_scope);
    };

    update_style_and_layout();

    // AD-HOC: A scroll-state() query against a container that has not been snapshotted yet reads no state. Like other
    //         engines, take such a container's first snapshot as soon as its layout is known, so that the style it
    //         decides is right before the next rendering update. Later changes of its state wait for that update. A
    //         document no such query asked about has nothing to snapshot, which is known without asking for layout.
    while (m_scroll_state_query_containers.has_containers() && layout_is_up_to_date() && m_scroll_state_query_containers.snapshot_post_layout_state(read, *this, CSS::ScrollStateQueryContainers::Snapshot::NewContainersOnly))
        update_style_and_layout();
}

// The first round of the rendering update's layout reads the document's facts as they are before its style flies, and
// runs after the style in the frame. List items that wait to be renumbered and top layer work are the layout update's
// to do first, so nothing is sealed while either waits.
void Document::seal_first_layout_round(Layout::BegunRead const& read)
{
    auto navigable = this->navigable();
    if (!navigable || navigable->active_document().ptr() != this || !m_layout_node_arena)
        return;
    if (!m_list_owners_pending_item_renumber.is_empty() || !m_elements_with_pending_top_layer_membership_change.is_empty() || m_top_layer_needs_layout_zone_rebuild)
        return;
    update_highlight_states_if_needed(read);
    Layout::RustFFI::FfiLayoutUpdateInputs inputs {
        .reason_is_inspect_devtools_layout_data = false,
        .is_template_contents_document = m_created_for_appropriate_template_contents,
        .reason_name = ffi_utf16_view(to_string(UpdateLayoutReason::HTMLEventLoopRenderingUpdate)),
    };
    Layout::RustFFI::render_state_seal_first_layout_round(m_layout_node_arena->host(), &read, &inputs);
}

void Document::update_style_and_layout_once(Layout::BegunRead const& read, UpdateLayoutReason reason, ThrottledAnimationSamplingScope animation_sampling_scope)
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
    Layout::RustFFI::document_host_begin_update_layout(arena.host());
    ScopeGuard guard = [&] {
        Layout::RustFFI::document_host_end_update_layout(arena.host());

        if (m_needs_scroll_container_resnap) {
            if (auto navigable = this->navigable(); navigable && navigable->active_document().ptr() == this)
                navigable->re_snap_scroll_containers_after_layout_change(read);
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
    Layout::RustFFI::render_state_update_layout(arena.host(), &read, &inputs);

    // A pass that reached a web face still waiting on its load cannot start the fetch itself: the fetch, the
    // font-display timer and the load-event delayer are all document state. It leaves the face's number behind
    // instead, and the request happens here, once the pass has ended and in the same rendering update. A face that
    // resolves as it is requested, as a local() one does, changes the fonts the pass picked.
    if (Gfx::request_wanted_pending_faces())
        m_requested_wanted_font_faces = true;

    // An <object> showing this document is sized from its <svg> document element, whose natural size only this
    // document's layout works out. Hand it over as it changes.
    if (auto* object = as_if<HTML::HTMLObjectElement>(navigable->container().ptr())) {
        Layout::RustFFI::FfiNaturalSize natural_size {};
        if (Layout::RustFFI::render_state_take_changed_document_svg_root_natural_size(arena.host(), &read, &natural_size)) {
            CSS::SizeWithAspectRatio size { natural_size.width, natural_size.height, {} };
            if (natural_size.has_aspect_ratio)
                size.aspect_ratio = CSSPixelFraction(natural_size.aspect_ratio_numerator, natural_size.aspect_ratio_denominator);
            object->set_natural_size_of_content_document(size);
        }
    }
}

}
