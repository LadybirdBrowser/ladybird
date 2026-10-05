/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeIdentity.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Painting/PaintFacts.h>

namespace Web::DOM {

// The paint facts the DOM side has left stale on the boxes, and the selection and search text states, which are read off
// the box and the DOM and style objects only the main thread holds, so they wait for the next read of the document,
// which finds the box beside a frame in flight. Every other mark on a box is queued for the render state at once (see
// Node::mark_box()). An entry names a node by identity, and a second note on the same node merges into the entry it
// already has. The document drains the journal before anything can observe what the notes did.
class WEB_API InvalidationJournal {
    AK_MAKE_NONCOPYABLE(InvalidationJournal);
    AK_MAKE_NONMOVABLE(InvalidationJournal);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit InvalidationJournal(Document& document)
        : m_document(document)
    {
    }

    void visit_edges(GC::Cell::Visitor&);

    // The node's facts of these families are stale. The drain reads them from the node.
    void note_paint_facts(NodeIdentity, Painting::PaintFactsFamily);
    // The image a box shows changed, in a task of its own that may run beside a frame in flight: the box's facts of
    // these families are stale, and it repaints. The box is named by its layout node, which asks the render state
    // nothing and names an anonymous box too. A box gone by the drain has nothing left to mark.
    void note_box_image_changed(Layout::Node&, Painting::PaintFactsFamily, InvalidateDisplayList);
    // The document's selection changed, or lost its range, so the selection states of the boxes it paints through are
    // stale.
    void note_selection_changed();
    // The document's active find-in-page match changed, or went away, so the search text states of the boxes it paints
    // through are stale.
    void note_search_text_changed();

    // The identity is retired and may name another node once it is handed out again, so what was noted for the node
    // that had it must not land on that one. Nothing may retire an identity while the journal drains.
    void forget(CSS::StyleNodeID);

    // Writes every entry through to the layout and paint state and empties the journal. Nearly every drain finds the
    // journal empty, which it answers without leaving the caller.
    void drain(Layout::BegunRead const& read)
    {
        if (!m_draining && has_marks())
            drain_marks(read);
    }

private:
    struct Entry {
        NodeIdentity identity {};
        // The box an entry names instead of a node, which identity then does not name.
        WeakPtr<Layout::Node> box {};
        InvalidateDisplayList invalidate_display_list { InvalidateDisplayList::No };
        Painting::PaintFactsFamily stale_paint_facts {};
    };

    Entry& entry_for(NodeIdentity);
    void drain_if_layout_is_reading();
    bool has_marks() const
    {
        return !m_entries.is_empty() || m_selection_changed || m_search_text_changed;
    }
    void drain_marks(Layout::BegunRead const&);

    GC::Ref<Document> m_document;
    Vector<Entry> m_entries;
    HashMap<NodeIdentity, size_t> m_entry_index_by_identity;
    bool m_draining { false };
    bool m_selection_changed { false };
    bool m_search_text_changed { false };
};

}
