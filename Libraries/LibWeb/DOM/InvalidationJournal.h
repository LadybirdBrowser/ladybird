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

// What the DOM side has marked dirty on the layout and paint state but has not written there yet. An entry names a
// node by identity and says what changed about it, and a second mark on the same node merges into the entry it already
// has, so ten writes to one element cost one mark. The document drains the journal before anything can observe what
// the marks did.
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

    void note_needs_layout_update(NodeIdentity, SetNeedsLayoutReason, Layout::LayoutUpdatePropagation);
    void note_needs_layout_tree_update(NodeIdentity, SetNeedsLayoutTreeUpdateReason);
    void note_needs_repaint(NodeIdentity, InvalidateDisplayList);
    void note_needs_repaint_in_subtree(NodeIdentity);
    void note_dom_paint_facts(NodeIdentity, u8 facts);
    void note_propagated_text_decoration_caches_invalidation(NodeIdentity);
    // The node's facts of these families are stale. The drain reads them from the node.
    void note_paint_facts(NodeIdentity, Painting::PaintFactsFamily);
    // The image a box shows changed, in a task of its own that may run beside a frame in flight: the box's facts of
    // these families are stale, and it repaints. The box is named by its layout node, which asks the render state
    // nothing and names an anonymous box too. A box gone by the drain has nothing left to mark.
    void note_box_image_changed(Layout::Node&, Painting::PaintFactsFamily, InvalidateDisplayList);
    // An image element's data changed, which changes its box as the box's kind and sizing decide.
    void note_image_data_changed(NodeIdentity, SetNeedsLayoutReason);
    // A text node's data changed, which changes its box as the box decides.
    void note_text_data_changed(NodeIdentity, bool whitespace_only_changed);

    // The identity is retired and may name another node once it is handed out again, so what was noted for the node
    // that had it must not land on that one. Nothing may retire an identity while the journal drains.
    void forget(CSS::StyleNodeID);

    // Writes every entry through to the layout and paint state and empties the journal.
    void drain(Layout::BegunRead const& read);

private:
    struct Entry {
        NodeIdentity identity {};
        // The box an entry names instead of a node, which identity then does not name.
        WeakPtr<Layout::Node> box {};
        // The reason of the first layout mark. Only the layout update trace reads it.
        SetNeedsLayoutReason layout_reason { SetNeedsLayoutReason::StyleChange };
        Layout::LayoutUpdatePropagation layout_propagation {};
        // The reason of the tree update mark that made the node dirty. A later mark on a node that is already dirty
        // changes nothing about the build, so only the first one's reason is kept.
        SetNeedsLayoutTreeUpdateReason layout_tree_update_reason { SetNeedsLayoutTreeUpdateReason::None };
        InvalidateDisplayList invalidate_display_list { InvalidateDisplayList::No };
        bool needs_layout_update { false };
        bool needs_layout_tree_update { false };
        bool needs_repaint { false };
        bool needs_subtree_repaint { false };
        bool invalidate_propagated_text_decoration_caches { false };
        bool has_dom_paint_facts { false };
        bool image_data_changed { false };
        bool text_data_changed { false };
        bool whitespace_only_text_changed { false };
        SetNeedsLayoutReason image_data_change_reason { SetNeedsLayoutReason::StyleChange };
        u8 dom_paint_facts { 0 };
        Painting::PaintFactsFamily stale_paint_facts {};
    };

    Entry& entry_for(NodeIdentity);
    void drain_if_layout_is_reading();

    GC::Ref<Document> m_document;
    Vector<Entry> m_entries;
    HashMap<NodeIdentity, size_t> m_entry_index_by_identity;
    bool m_draining { false };
};

}
