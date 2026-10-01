/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void InvalidationJournal::note_paint_facts(NodeIdentity identity, Painting::PaintFactsFamily families)
{
    entry_for(identity).stale_paint_facts |= families;
    drain_if_layout_is_reading();
}

void InvalidationJournal::forget(CSS::StyleNodeID style_node)
{
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
    if (m_document->is_running_update_layout())
        drain();
}

void InvalidationJournal::drain()
{
    while (!m_entries.is_empty()) {
        auto entries = move(m_entries);
        m_entry_index_by_identity.clear_with_capacity();

        if (auto* arena = m_document->layout_node_arena_if_created()) {
            for (auto const& entry : entries) {
                // A node whose box went away between the mark and here has nothing left to mark.
                auto* layout_node = entry.identity.bound_layout_node(*arena);
                if (!layout_node)
                    continue;
                if (entry.needs_layout_update)
                    layout_node->set_needs_layout_update(entry.layout_reason, entry.layout_propagation);
                auto needs_repaint = entry.needs_repaint;
                auto invalidate_display_list = entry.invalidate_display_list;
                if (entry.has_dom_paint_facts && Layout::RustFFI::layout_arena_set_node_dom_paint_facts(arena->handle(), Layout::Node::slot_id(layout_node), entry.dom_paint_facts)) {
                    needs_repaint = true;
                    invalidate_display_list = InvalidateDisplayList::PaintCommandsAndHitTestList;
                }
                if (entry.stale_paint_facts != Painting::PaintFactsFamily::None)
                    Painting::apply_paint_facts(*layout_node, entry.stale_paint_facts);
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
