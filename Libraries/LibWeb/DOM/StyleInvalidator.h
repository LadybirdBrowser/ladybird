/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class StyleInvalidator : public GC::Cell {
    GC_CELL(StyleInvalidator, GC::Cell);
    GC_DECLARE_ALLOCATOR(StyleInvalidator);

public:
    void invalidate(Node& node);
    bool enqueue_invalidation_plan(Node&, StyleInvalidationReason, CSS::InvalidationPlan const&);
    bool has_pending_invalidations() const { return !m_pending_invalidations.is_empty(); }

    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    struct PendingDescendantInvalidation {
        StyleInvalidationReason reason;
        CSS::DescendantInvalidationRule rule;

        bool operator==(PendingDescendantInvalidation const&) const = default;
    };

    void add_pending_invalidation(GC::Ref<Node>, StyleInvalidationReason, CSS::InvalidationPlan const&);
    void apply_invalidation_plan(Element&, StyleInvalidationReason, CSS::InvalidationPlan const&, bool& invalidate_entire_subtree);
    void apply_sibling_invalidation(Element&, StyleInvalidationReason, CSS::SiblingInvalidationRule const&);
    void perform_pending_style_invalidations(Node& node, bool invalidate_entire_subtree);

    HashMap<GC::Ref<Node>, Vector<PendingDescendantInvalidation>> m_pending_invalidations;
    Vector<PendingDescendantInvalidation> m_active_descendant_invalidations;
};

}
