/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/DOM/Slottable.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-slot
class Slot {
public:
    virtual ~Slot();

    String const& slot_name() const { return m_name; } // Not called `name` to distinguish from `Element::name`.
    void set_slot_name(String name) { m_name = move(name); }

    ReadonlySpan<DOM::Slottable> assigned_nodes_internal() const { return m_assigned_nodes; }
    void set_assigned_nodes(Vector<DOM::Slottable> assigned_nodes) { m_assigned_nodes = move(assigned_nodes); }

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    // https://dom.spec.whatwg.org/#slot-name
    String m_name;

    // https://dom.spec.whatwg.org/#slot-assigned-nodes
    Vector<Slottable> m_assigned_nodes;
};

}
