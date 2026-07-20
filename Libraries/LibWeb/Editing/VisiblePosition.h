/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Selection/CaretNavigation.h>

namespace Web::Editing {

// A rendered editing position keeps the script-visible DOM boundary separate from its canonical caret equivalent.
// Structural editing decisions use the former to retain explicit br boundaries, while selection snapshots and caret
// painting can use the latter. This is the same distinction represented by Position and VisiblePosition in Blink and
// WebKit, and by EditorDOMPoint plus layout-aware scanners in Gecko.
class VisiblePosition {
public:
    static VisiblePosition create(DOM::Document&, DOM::BoundaryPoint, TextAffinity = TextAffinity::Downstream);

    DOM::BoundaryPoint boundary() const { return { m_boundary.node, static_cast<WebIDL::UnsignedLong>(m_boundary.offset) }; }
    DOM::BoundaryPoint deep_equivalent() const { return { m_deep_equivalent.node, static_cast<WebIDL::UnsignedLong>(m_deep_equivalent.offset) }; }
    TextAffinity affinity() const { return m_boundary.affinity; }

    Optional<VisiblePosition> next() const;
    Optional<VisiblePosition> previous() const;
    Optional<VisiblePosition> move(Web::Selection::SelectionAlteration, Web::Selection::SelectionDirection, Web::Selection::SelectionGranularity, Optional<CSSPixels> preferred_inline_coordinate = {}) const;
    Optional<CSSPixels> inline_coordinate() const;
    Optional<DOM::BoundaryPoint> canonical_boundary_for_extension(Web::Selection::SelectionDirection) const;

    bool is_start_of_paragraph() const;
    bool is_end_of_paragraph() const;
    bool is_start_of_containing_block() const;
    bool is_end_of_containing_block() const;
    bool is_before_or_after_containing_block() const;

private:
    VisiblePosition(DOM::Document&, Web::Selection::CaretLocation boundary, Web::Selection::CaretLocation deep_equivalent);

    GC::Ref<DOM::Document> m_document;
    Web::Selection::CaretLocation m_boundary;
    Web::Selection::CaretLocation m_deep_equivalent;
};

}
