/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Editing/InsertedNodes.h>
#include <LibWeb/Editing/MutationTrackedRange.h>
#include <LibWeb/Editing/ReplacementEndpoints.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// Tracks both the structural edges of inserted content and the exact destination seam where it was inserted. These are
// intentionally distinct: paragraph integration moves the content edges, while whitespace repair still needs the
// mutation-adjusted destination seam instead of reconstructing it from the resulting tree.
class InsertedContent {
    AK_MAKE_NONCOPYABLE(InsertedContent);

public:
    enum class ReplacementTopology {
        Inline,
        Block,
    };

    explicit InsertedContent(GC::RootVector<GC::Ref<DOM::Node>>&&);
    InsertedContent(InsertedContent&&) = default;

    bool is_empty() const { return m_nodes.is_empty(); }
    GC::RootVector<GC::Ref<DOM::Node>> nodes() const;
    GC::Ref<DOM::Node> first_node() const;
    GC::Ref<DOM::Node> last_node() const;
    GC::RootVector<GC::Ref<DOM::Text>> text_nodes() const;
    bool contains(DOM::Text const&) const;
    DOM::BoundaryPoint insertion_boundary() const;
    DOM::BoundaryPoint start_boundary() const;
    DOM::BoundaryPoint completion_start_boundary() const;
    DOM::BoundaryPoint end_boundary() const;
    Optional<DOM::BoundaryPoint> end_boundary_after_atomic_content() const;
    ReplacementTopology replacement_topology() const
    {
        VERIFY(m_replacement_topology.has_value());
        return *m_replacement_topology;
    }
    bool insertion_was_inside_paragraph() const { return m_insertion_was_inside_paragraph; }
    bool insertion_was_after_whitespace() const { return m_insertion_was_after_whitespace; }
    bool end_paragraph_was_integrated() const { return m_end_paragraph_was_integrated; }

    ReplacementEndpoints& endpoints()
    {
        VERIFY(m_endpoints);
        return *m_endpoints;
    }
    void begin_completion(DOM::BoundaryPoint end);

    void insert_before(DOM::Node& parent, GC::Ptr<DOM::Node> reference_node);
    void insert_into_range(DOM::Range&, DOM::DocumentFragment&);
    void track_replacement_boundary(DOM::BoundaryPoint, ReplacementTopology);
    void begin_tracking_content_boundaries();
    void did_replace_node(DOM::Node&, DOM::Node& replacement);
    void did_replace_node(DOM::Node&, DOM::Node& first_replacement, DOM::Node& last_replacement);
    void did_move_start_paragraph(DOM::BoundaryPoint);
    void will_integrate_end_paragraph(DOM::Node&);
    void require_end_text_seam_normalization() { m_normalize_end_text_seam = true; }
    bool should_normalize_end_text_seam() const { return m_normalize_end_text_seam; }

private:
    void track_insertion_boundary(DOM::BoundaryPoint);

    InsertedNodes m_nodes;
    OwnPtr<MutationTrackedRange> m_replacement_range;
    OwnPtr<MutationTrackedRange> m_insertion_range;
    OwnPtr<ReplacementEndpoints> m_endpoints;
    Optional<ReplacementTopology> m_replacement_topology;
    bool m_start_paragraph_was_integrated { false };
    bool m_insertion_was_inside_paragraph { false };
    bool m_insertion_was_after_whitespace { false };
    bool m_replacement_was_inside_inline_content { false };
    bool m_end_paragraph_was_integrated { false };
    bool m_normalize_end_text_seam { false };
};

}
