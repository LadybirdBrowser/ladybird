/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class StyleInvalidator : public GC::Cell {
    GC_CELL(StyleInvalidator, GC::Cell);
    GC_DECLARE_ALLOCATOR(StyleInvalidator);

public:
    void invalidate(Node& node);
    void add_pending_invalidation(GC::Ref<Node>, CSS::InvalidationSet&&, bool invalidate_elements_that_use_css_custom_properties);
    bool has_pending_invalidations() const { return !m_pending_invalidations.is_empty(); }

    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    void perform_pending_style_invalidations(Node& node, bool invalidate_entire_subtree);

    struct PendingInvalidation {
        bool invalidate_elements_that_use_css_custom_properties { false };
        CSS::InvalidationSet invalidation_set;
    };
    HashMap<GC::Ref<Node>, PendingInvalidation> m_pending_invalidations;

    Vector<CSS::InvalidationSet> m_subtree_invalidation_sets;
    bool m_invalidate_elements_that_use_css_custom_properties { false };
};

}
