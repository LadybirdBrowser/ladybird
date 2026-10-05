/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/InvalidationJournal.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::DOM {

void InvalidationJournal::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
    visitor.visit(m_language_changed_roots);
    visitor.visit(m_editability_changed_roots);
}

InvalidationJournal::Entry& InvalidationJournal::entry_for(NodeIdentity identity)
{
    auto index = m_entry_index_by_identity.ensure(identity, [&] {
        m_entries.append(Entry { .identity = identity });
        return m_entries.size() - 1;
    });
    return m_entries[index];
}

void InvalidationJournal::note_needs_layout_update(NodeIdentity identity, SetNeedsLayoutReason reason, Layout::LayoutUpdatePropagation propagation)
{
    auto& entry = entry_for(identity);
    if (!entry.needs_layout_update) {
        entry.needs_layout_update = true;
        entry.layout_reason = reason;
        entry.layout_propagation = propagation;
    } else if (propagation == Layout::LayoutUpdatePropagation::ThroughAncestors) {
        // Marking the ancestors as well covers marking only the node, so the wider mark wins.
        entry.layout_propagation = propagation;
    }
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_needs_layout_tree_update(NodeIdentity identity, SetNeedsLayoutTreeUpdateReason reason)
{
    auto& entry = entry_for(identity);
    if (!entry.needs_layout_tree_update) {
        entry.needs_layout_tree_update = true;
        entry.layout_tree_update_reason = reason;
    }
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_needs_repaint(NodeIdentity identity, InvalidateDisplayList invalidate_display_list)
{
    auto& entry = entry_for(identity);
    entry.needs_repaint = true;
    // Each level of display list invalidation covers the one below it, so the widest mark wins.
    entry.invalidate_display_list = max(entry.invalidate_display_list, invalidate_display_list);
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_needs_repaint_in_subtree(NodeIdentity identity)
{
    auto& entry = entry_for(identity);
    entry.needs_subtree_repaint = true;
    entry.needs_repaint = true;
    entry.invalidate_display_list = InvalidateDisplayList::PaintCommandsAndHitTestList;
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_dom_paint_facts(NodeIdentity identity, u8 facts)
{
    auto& entry = entry_for(identity);
    entry.has_dom_paint_facts = true;
    entry.dom_paint_facts = facts;
    // Facts that changed repaint the node when the journal drains, and the frame that drains it is asked for now.
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_propagated_text_decoration_caches_invalidation(NodeIdentity identity)
{
    entry_for(identity).invalidate_propagated_text_decoration_caches = true;
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_paint_facts(NodeIdentity identity, Painting::PaintFactsFamily families)
{
    auto& entry = entry_for(identity);
    // Noting that the box has no layer image facts replaces a pending layer image update, and the other way around.
    constexpr auto LAYER_IMAGE_FAMILIES = Painting::PaintFactsFamily::LayerImages | Painting::PaintFactsFamily::NoLayerImages;
    if (has_any_flag(families, LAYER_IMAGE_FAMILIES))
        entry.stale_paint_facts &= ~LAYER_IMAGE_FAMILIES;
    entry.stale_paint_facts |= families;
    // Replaced image and video facts that changed repaint the box when the journal drains, and the frame that drains
    // them is asked for now.
    if (has_any_flag(families, Painting::PaintFactsFamily::ReplacedImage | Painting::PaintFactsFamily::Video))
        m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_box_image_changed(Layout::Node& box, Painting::PaintFactsFamily families, InvalidateDisplayList invalidate_display_list)
{
    m_entries.append(Entry {
        .box = box,
        .invalidate_display_list = invalidate_display_list,
        .needs_repaint = invalidate_display_list != InvalidateDisplayList::No,
        .stale_paint_facts = families,
    });
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_image_data_changed(NodeIdentity identity, SetNeedsLayoutReason reason)
{
    auto& entry = entry_for(identity);
    entry.image_data_changed = true;
    entry.image_data_change_reason = reason;
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_text_data_changed(NodeIdentity identity, bool whitespace_only_changed)
{
    auto& entry = entry_for(identity);
    entry.text_data_changed = true;
    entry.whitespace_only_text_changed |= whitespace_only_changed;
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_language_changed(Element& element)
{
    if (!m_language_changed_roots.contains_slow(GC::Ref { element }))
        m_language_changed_roots.append(element);
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_editability_changed(Node& node)
{
    if (!m_editability_changed_roots.contains_slow(GC::Ref { node }))
        m_editability_changed_roots.append(node);
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_top_layer_boxes_repaint(NodeIdentity identity)
{
    entry_for(identity).needs_backdrop_repaint = true;
    note_needs_repaint_in_subtree(identity);
}

void InvalidationJournal::note_selection_changed()
{
    m_selection_changed = true;
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::note_search_text_changed()
{
    m_search_text_changed = true;
    m_document->request_frame_for_pending_repaint({});
    drain_if_layout_is_reading();
}

void InvalidationJournal::forget(CSS::StyleNodeID style_node)
{
    // A drain holds the generation it writes through outside the index, where forgetting cannot reach it, so an
    // identity retired mid-drain could still have its entry land on the node it is handed to next.
    VERIFY(!m_draining);
    auto index = m_entry_index_by_identity.take(NodeIdentity::of_style_node(style_node));
    if (!index.has_value())
        return;
    // The last entry takes the forgotten one's place, so that the index moves for that entry alone, and a journal no
    // drain empties, as that of a document no event loop renders, holds no more entries than the nodes it names at once.
    auto last = m_entries.take_last();
    if (*index == m_entries.size())
        return;
    if (last.identity)
        m_entry_index_by_identity.set(last.identity, *index);
    m_entries[*index] = move(last);
}

// A mark made from inside a layout update is one the update is about to read, so it goes through at once. Outside
// one, nothing reads what these marks change before the next drain.
void InvalidationJournal::drain_if_layout_is_reading()
{
    if (!m_document->is_running_update_layout())
        return;
    // The drain belongs to the read the layout update began.
    Layout::ForcedReadScope read { *m_document };
    drain(read);
}

// Whether the reason describes a mutation that only affects the node's children and can never
// change the node's own box kind, so a rebuild on a partial relayout boundary stays confined
// to its subtree. Reasons not classified here forfeit partial relayout for their mutations.
static bool is_structural_boundary_self_rebuild_reason(SetNeedsLayoutTreeUpdateReason reason)
{
    switch (reason) {
    case SetNeedsLayoutTreeUpdateReason::NodeInsertBefore:
    case SetNeedsLayoutTreeUpdateReason::NodeRemove:
    case SetNeedsLayoutTreeUpdateReason::NodeSetTextContent:
    case SetNeedsLayoutTreeUpdateReason::CharacterDataReplaceData:
    case SetNeedsLayoutTreeUpdateReason::ElementSetInnerHTML:
    case SetNeedsLayoutTreeUpdateReason::ShadowRootSetInnerHTML:
    case SetNeedsLayoutTreeUpdateReason::SlotAssignmentChange:
    // The box of an element that entered the top layer leaves the parent's subtree,
    // which is a child-list change.
    case SetNeedsLayoutTreeUpdateReason::TopLayerMembershipChange:
        return true;
    default:
        return false;
    }
}

// What a layout tree update mark made for `reason` asks of the box it marks, which the render state applies: whether the
// box relays out alone, defers to the insertion, or dirties its ancestors, and whether the rebuild has to climb past
// anonymous parents, reads the layout tree.
static Layout::RustFFI::FfiLayoutTreeUpdateMark layout_tree_update_mark(SetNeedsLayoutTreeUpdateReason reason)
{
    return {
        .reuse_reason = Node::layout_tree_update_reuse_reason(reason),
        .is_child_list_insertion = reason == SetNeedsLayoutTreeUpdateReason::NodeInsertBefore,
        .is_structural_boundary_self_rebuild = is_structural_boundary_self_rebuild_reason(reason),
    };
}

void InvalidationJournal::drain_marks(Layout::BegunRead const& read)
{
    // What the drain writes can mark more, and those marks land in the next generation of entries, which the loop below
    // drains in turn.
    TemporaryChange draining { m_draining, true };

    if (exchange(m_selection_changed, false) && m_document->has_committed_viewport_box()) {
        // The selection's range now decides the states, or a selection without one takes the highlight back. A document
        // without a browsing context has no selection.
        auto selection = m_document->get_selection();
        if (auto range = selection ? selection->range() : nullptr)
            m_document->paint_state().recompute_selection_states(read, *m_document, *range);
        else
            m_document->paint_state().reset_selection_states(read, *m_document);
        static_cast<Node&>(*m_document).set_needs_repaint(InvalidateDisplayList::PaintCommands);
    }

    if (exchange(m_search_text_changed, false) && m_document->has_committed_viewport_box()) {
        m_document->recompute_search_text_paint_states(read);
        static_cast<Node&>(*m_document).set_needs_repaint(InvalidateDisplayList::PaintCommands);
    }

    for (auto& root : exchange(m_language_changed_roots, {}))
        CSS::Invalidation::enroll_text_after_language_change(read, root);
    for (auto& root : exchange(m_editability_changed_roots, {}))
        root->apply_editability_to_boxes({}, read);

    while (!m_entries.is_empty()) {
        auto entries = move(m_entries);
        // The index holds the identities of these entries alone, so taking them out of it costs what the entries
        // number. Clearing it would zero every bucket it ever grew to, and a layout update drains once per mark.
        for (auto const& entry : entries)
            m_entry_index_by_identity.remove(entry.identity);
        VERIFY(m_entry_index_by_identity.is_empty());

        if (auto* arena = m_document->layout_node_arena_if_created()) {
            for (auto const& entry : entries) {
                // The render state finds the node's box as it applies a tree update, which goes first, so that a
                // rebuild it escalates to an ancestor is known before the node's other marks. The document is named by 0.
                if (entry.needs_layout_tree_update)
                    Layout::RustFFI::render_state_apply_layout_tree_update_mark(arena->host(), entry.identity.style_node().value(), layout_tree_update_mark(entry.layout_tree_update_reason));
                if (!entry.has_marks_for_box())
                    continue;
                // A node whose box went away between the mark and here has nothing left to mark, nor has a box that did.
                auto* layout_node = entry.box ? entry.box.ptr() : entry.identity.bound_layout_node(read, *arena);
                if (!layout_node)
                    continue;
                if (entry.text_data_changed) {
                    if (auto* text_node = as_if<Layout::TextNode>(*layout_node))
                        as<CharacterData>(*layout_node->dom_node()).apply_text_data_change({}, *text_node, entry.whitespace_only_text_changed);
                }
                if (entry.image_data_changed) {
                    if (auto* image = as_if<HTML::HTMLImageElement>(layout_node->dom_node()))
                        image->apply_image_data_change({}, *layout_node, entry.image_data_change_reason);
                }
                if (entry.needs_layout_update)
                    layout_node->set_needs_layout_update(entry.layout_reason, entry.layout_propagation);
                auto needs_repaint = entry.needs_repaint;
                auto invalidate_display_list = entry.invalidate_display_list;
                // The rows take the facts the DOM node noted, which they are painted and hit-tested with again.
                if (entry.has_dom_paint_facts) {
                    Layout::RustFFI::render_state_set_dom_paint_facts(arena->host(), Layout::Node::slot_id(layout_node), entry.dom_paint_facts);
                    needs_repaint = true;
                    invalidate_display_list = InvalidateDisplayList::PaintCommandsAndHitTestList;
                }
                if (entry.stale_paint_facts != Painting::PaintFactsFamily::None)
                    Painting::apply_paint_facts(*layout_node, entry.stale_paint_facts);
                if (entry.invalidate_propagated_text_decoration_caches)
                    Painting::apply_paint_cache_invalidation(*layout_node, Painting::PaintCacheInvalidation::PropagatedTextDecorations);
                if (entry.needs_subtree_repaint)
                    Painting::apply_subtree_repaint_damage(*layout_node);
                if (entry.needs_backdrop_repaint) {
                    if (auto* element = as_if<Element>(layout_node->dom_node())) {
                        if (auto* backdrop_layout_node = element->pseudo_element_unsafe_layout_node(read, CSS::PseudoElement::Backdrop))
                            Painting::set_needs_repaint_in_subtree(*backdrop_layout_node);
                    }
                }
                if (needs_repaint) {
                    if (auto* text_node = as_if<Layout::TextNode>(*layout_node))
                        Painting::apply_text_repaint_damage(*text_node, invalidate_display_list);
                    else
                        Painting::apply_repaint_damage(*layout_node, invalidate_display_list);
                }
            }
        }

        // The next generation reuses the storage, unless what the drain wrote through noted more.
        if (m_entries.is_empty()) {
            entries.clear_with_capacity();
            m_entries = move(entries);
        }
    }
}

}
