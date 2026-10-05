/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// The selection and search text states the DOM side has left stale on the boxes, which are found from the DOM, so they
// wait for the next read of the document, which finds the boxes beside a frame in flight. Every mark on a box is queued
// for the render state at once (see Node::mark_box()). The document drains the journal before anything can observe what
// the notes did.
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

    // The document's selection changed, or lost its range, so the selection states of the boxes it paints through are
    // stale.
    void note_selection_changed();
    // The document's active find-in-page match changed, or went away, so the search text states of the boxes it paints
    // through are stale.
    void note_search_text_changed();

    // Writes the stale states through to the paint state. Nearly every drain finds the journal empty, which it answers
    // without leaving the caller.
    void drain(Layout::BegunRead const& read)
    {
        if (m_selection_changed || m_search_text_changed)
            drain_marks(read);
    }

private:
    void drain_if_layout_is_reading();
    void drain_marks(Layout::BegunRead const&);

    GC::Ref<Document> m_document;
    bool m_selection_changed { false };
    bool m_search_text_changed { false };
};

}
