/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class SlotRegistry {
public:
    void add(HTML::HTMLSlotElement&);
    void remove(HTML::HTMLSlotElement&);
    GC::Ptr<HTML::HTMLSlotElement> first_slot_with_name(FlyString const& name) const;

    template<typename Callback>
    void for_each_slot(Callback callback)
    {
        if (is_empty())
            return;

        for (auto& slot : m_slots) {
            if (slot)
                callback(*slot);
        }
    }

    bool is_empty() const;

private:
    bool try_insert_in_tree_order(HTML::HTMLSlotElement&);

    Vector<GC::Weak<HTML::HTMLSlotElement>> m_slots;
    size_t m_slot_count { 0 };
};

}
