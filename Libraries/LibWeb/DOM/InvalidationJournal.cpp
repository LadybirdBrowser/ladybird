/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/InvalidationJournal.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::DOM {

void InvalidationJournal::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
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
}

}
