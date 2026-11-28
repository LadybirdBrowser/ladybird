/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-slotable
using Slottable = Variant<GC::Ref<Element>, GC::Ref<Text>>;

// https://dom.spec.whatwg.org/#mixin-slotable
class WEB_API SlottableMixin {
public:
    virtual ~SlottableMixin();

    virtual Node& slottable_as_node() = 0;

    GC::Ptr<HTML::HTMLSlotElement> assigned_slot();

    GC::Ptr<HTML::HTMLSlotElement> assigned_slot_internal() const;
    void set_assigned_slot(GC::Ptr<HTML::HTMLSlotElement> assigned_slot);

    GC::Ptr<HTML::HTMLSlotElement> manual_slot_assignment();
    void set_manual_slot_assignment(GC::Ptr<HTML::HTMLSlotElement> manual_slot_assignment);

protected:
    struct RareData {
        void visit_edges(JS::Cell::Visitor&);

        // https://dom.spec.whatwg.org/#slottable-manual-slot-assignment
        GC::Ptr<HTML::HTMLSlotElement> manual_slot_assignment;
    };

    virtual RareData* slottable_rare_data() = 0;
    virtual RareData const* slottable_rare_data() const = 0;
    virtual RareData& ensure_slottable_rare_data() = 0;
};

// Defined in HTML/HTMLSlotElement.h.
inline GC::Ptr<HTML::HTMLSlotElement> assigned_slot_for_node(GC::Ref<Node>);

}
