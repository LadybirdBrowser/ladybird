/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#mixin-slotable
class SlottableMixin {
public:
    virtual ~SlottableMixin();

    virtual DOM::Node const& slottable_as_node() const = 0;

    String const& slottable_name() const { return m_name; } // Not called `name` to distinguish from `Element::name`.
    void set_slottable_name(String name) { m_name = move(name); }

    JS::GCPtr<HTML::HTMLSlotElement> assigned_slot();

    JS::GCPtr<HTML::HTMLSlotElement> assigned_slot_internal() const { return m_assigned_slot; }
    void set_assigned_slot(JS::GCPtr<HTML::HTMLSlotElement> assigned_slot) { m_assigned_slot = assigned_slot; }

    JS::GCPtr<HTML::HTMLSlotElement> manual_slot_assignment() { return m_manual_slot_assignment; }
    void set_manual_slot_assignment(JS::GCPtr<HTML::HTMLSlotElement> manual_slot_assignment) { m_manual_slot_assignment = manual_slot_assignment; }

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    // https://dom.spec.whatwg.org/#slotable-name
    String m_name;

    // https://dom.spec.whatwg.org/#slotable-assigned-slot
    JS::GCPtr<HTML::HTMLSlotElement> m_assigned_slot;

    // https://dom.spec.whatwg.org/#slottable-manual-slot-assignment
    JS::GCPtr<HTML::HTMLSlotElement> m_manual_slot_assignment;

    IntrusiveListNode<SlottableMixin> m_list_node;

public:
    using List = IntrusiveList<&SlottableMixin::m_list_node>;
};

enum class OpenFlag {
    Set,
    Unset,
};

JS::GCPtr<HTML::HTMLSlotElement> assigned_slot_for_node(JS::NonnullGCPtr<Node>);
bool is_an_assigned_slottable(JS::NonnullGCPtr<Node>);

JS::GCPtr<HTML::HTMLSlotElement> find_a_slot(SlottableMixin const&, OpenFlag = OpenFlag::Unset);
void assign_slottables(JS::NonnullGCPtr<HTML::HTMLSlotElement>);
void assign_slottables_for_a_tree(JS::NonnullGCPtr<Node>);
void assign_a_slot(SlottableMixin const&);
void signal_a_slot_change(JS::NonnullGCPtr<HTML::HTMLSlotElement>);

}
