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
        .stale_paint_facts = families,
    });
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
                // A node whose box went away between the mark and here has nothing left to mark, nor has a box that did.
                auto* layout_node = entry.box ? entry.box.ptr() : entry.identity.bound_layout_node(read, *arena);
                if (!layout_node)
                    continue;
                if (entry.stale_paint_facts != Painting::PaintFactsFamily::None)
                    Painting::apply_paint_facts(*layout_node, entry.stale_paint_facts);
                if (entry.invalidate_display_list != InvalidateDisplayList::No)
                    Painting::set_needs_repaint(*layout_node, entry.invalidate_display_list);
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
