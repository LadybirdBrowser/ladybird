/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::DOM {

// The flags of a node that only C++ reads and writes, which the node keeps in the last byte of its core (see
// Rust/src/dom/node_core.rs), beside the core's own flags, so that DOM::Node does not grow.
struct CppNodeFlags {
    bool has_layout_box : 1 { false };
    bool has_committed_box : 1 { false };
    // OPTIMIZATION: Whether an element or Text node holds an assigned slot in its core, see Node::has_assigned_slot()
    //               and SlottableMixin::set_assigned_slot().
    bool has_assigned_slot : 1 { false };
    bool inside_blocking_wheel_event_handler : 1 { false };
    bool has_spelling_markers : 1 { false };
    bool style_arrival_pending : 1 { false };
    bool descendant_style_arrival_pending : 1 { false };
    bool child_style_arrivals_wait : 1 { false };
};

static_assert(sizeof(CppNodeFlags) == 1 && alignof(CppNodeFlags) == 1);

}
