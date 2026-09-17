/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-conditional-5/#scroll-state-container
// The state a scroll-state() query reads. stuck, scrollable and scrolled are sets of physical edges, and snapped is a
// set of physical axes, encoded with the SCROLL_STATE_EDGE_* and SCROLL_STATE_SNAPPED_* bits of the query parser.
struct ScrollStateSnapshot {
    u8 stuck { 0 };
    u8 snapped { 0 };
    u8 scrollable { 0 };
    u8 scrolled { 0 };

    bool operator==(ScrollStateSnapshot const&) const = default;
};

// The scroll-state query containers of a document. A query does not read a container's live scroll state, but the
// snapshot taken after layout in the last rendering update, so that the style it decides cannot feed back into the
// state it read within one style and layout pass.
class ScrollStateQueryContainers {
public:
    // The snapshot a scroll-state() query against the container reads. The container is snapshotted from the first
    // query on, so a container queried for the first time reads no state until the next snapshot.
    ScrollStateSnapshot snapshot_for_query(DOM::Element& container);

    // Records the direction of a relative scroll of a scrolling box by the delta, which scroll-state(scrolled) reads
    // for the element whose scrolling box it is. The direction is kept whether or not the element is a query container
    // yet, so a query that starts asking later reads it too.
    void did_scroll_relatively(Layout::Node const& scrolling_box, CSSPixelPoint delta);

    enum class Snapshot : u8 {
        AllContainers,
        NewContainersOnly,
    };

    // https://github.com/whatwg/html/pull/11613
    // Snapshots the scroll state of the containers, invalidating the styles that queried a state that changed. Returns
    // whether any did.
    bool snapshot_post_layout_state(DOM::Document&, Snapshot);

    void visit_edges(GC::Cell::Visitor&);

private:
    struct Container {
        ScrollStateSnapshot snapshot;
        bool has_been_snapshotted { false };
    };

    HashMap<GC::Ref<DOM::Element>, Container> m_containers;
    u8 m_viewport_last_relative_scroll_direction { 0 };
};

}
