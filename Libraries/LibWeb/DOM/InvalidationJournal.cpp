/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/InvalidationJournal.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintFacts.h>

namespace Web::DOM {

void InvalidationJournal::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
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

void InvalidationJournal::forget(CSS::StyleNodeID style_node)
{
    // A drain holds the generation it writes through outside the index, where forgetting cannot reach it, so an
    // identity retired mid-drain could still have its entry land on the node it is handed to next.
    VERIFY(!m_draining);
    auto index = m_entry_index_by_identity.take(NodeIdentity::of_style_node(style_node));
    if (!index.has_value())
        return;
    // The entry keeps its place so that the index stays valid for the entries after it. Naming no node, it drains
    // nowhere.
    m_entries[*index] = {};
}

// A mark made from inside a layout update is one the update is about to read, so it goes through at once. Outside
// one, nothing reads what these marks change before the next drain.
void InvalidationJournal::drain_if_layout_is_reading()
{
    if (!m_document->is_running_update_layout())
        return;
    // The drain belongs to the read the layout update began.
    Layout::ForcedReadScope read { *m_document, false };
    drain(read);
}

void InvalidationJournal::drain(Layout::BegunRead const& read)
{
    // What the drain writes can mark more, and those marks land in the next generation of entries, which the loop below
    // drains in turn.
    if (m_draining)
        return;
    TemporaryChange draining { m_draining, true };

    while (!m_entries.is_empty()) {
        auto entries = move(m_entries);
        // The index holds the identities of these entries alone, so taking them out of it costs what the entries
        // number. Clearing it would zero every bucket it ever grew to, and a layout update drains once per mark.
        for (auto const& entry : entries)
            m_entry_index_by_identity.remove(entry.identity);
        VERIFY(m_entry_index_by_identity.is_empty());

        if (auto* arena = m_document->layout_node_arena_if_created()) {
            for (auto const& entry : entries) {
                // A node whose box went away between the mark and here has nothing left to mark, nor has a box that did.
                auto* layout_node = entry.box ? entry.box.ptr() : entry.identity.bound_layout_node(read, *arena);
                if (!layout_node)
                    continue;
                // The tree update goes first, so that a rebuild it escalates to an ancestor is known before the
                // node's other marks.
                if (entry.needs_layout_tree_update)
                    layout_node->dom_node()->apply_layout_tree_update_mark(*layout_node, entry.layout_tree_update_reason);
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
