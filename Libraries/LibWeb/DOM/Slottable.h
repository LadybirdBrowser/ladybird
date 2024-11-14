/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-slotable
using Slottable = Variant<GC::Ref<Element>, GC::Ref<Text>>;

// https://dom.spec.whatwg.org/#mixin-slotable
class SlottableMixin {
public:
    virtual ~SlottableMixin();

    String const& slottable_name() const { return m_name; } // Not called `name` to distinguish from `Element::name`.
    void set_slottable_name(String name) { m_name = move(name); }

    GC::Ptr<HTML::HTMLSlotElement> assigned_slot();

    GC::Ptr<HTML::HTMLSlotElement> assigned_slot_internal() const { return m_assigned_slot; }
    void set_assigned_slot(GC::Ptr<HTML::HTMLSlotElement> assigned_slot) { m_assigned_slot = assigned_slot; }

    GC::Ptr<HTML::HTMLSlotElement> manual_slot_assignment() { return m_manual_slot_assignment; }
    void set_manual_slot_assignment(GC::Ptr<HTML::HTMLSlotElement> manual_slot_assignment) { m_manual_slot_assignment = manual_slot_assignment; }

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    // https://dom.spec.whatwg.org/#slotable-name
    String m_name;

    // https://dom.spec.whatwg.org/#slotable-assigned-slot
    GC::Ptr<HTML::HTMLSlotElement> m_assigned_slot;

    // https://dom.spec.whatwg.org/#slottable-manual-slot-assignment
    GC::Ptr<HTML::HTMLSlotElement> m_manual_slot_assignment;
};

enum class OpenFlag {
    Set,
    Unset,
};

GC::Ptr<HTML::HTMLSlotElement> assigned_slot_for_node(GC::Ref<Node>);
bool is_an_assigned_slottable(GC::Ref<Node>);

GC::Ptr<HTML::HTMLSlotElement> find_a_slot(Slottable const&, OpenFlag = OpenFlag::Unset);
Vector<Slottable> find_slottables(GC::Ref<HTML::HTMLSlotElement>);
void assign_slottables(GC::Ref<HTML::HTMLSlotElement>);
void assign_slottables_for_a_tree(GC::Ref<Node>);
void assign_a_slot(Slottable const&);
void signal_a_slot_change(GC::Ref<HTML::HTMLSlotElement>);

}
