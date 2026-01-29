/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/SlotRegistry.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::DOM {

void SlotRegistry::add(HTML::HTMLSlotElement& slot)
{
    m_slot_count -= m_slots.remove_all_matching([](GC::Weak<HTML::HTMLSlotElement>& s) {
        return !s;
    });

    auto slot_removed = m_slots.remove_first_matching([&](auto const& other_slot) {
        return &slot == other_slot.ptr();
    });
    if (slot_removed)
        --m_slot_count;

    // Insert in tree order.
    if (m_slots.is_empty() || m_slots.last()->is_before(slot)) {
        m_slots.append(GC::Weak { slot });
    } else {
        m_slots.insert_before_matching(GC::Weak { slot }, [&](auto& other_slot) {
            return slot.is_before(*other_slot);
        });
    }
    ++m_slot_count;
}

void SlotRegistry::remove(HTML::HTMLSlotElement& slot)
{
    m_slot_count -= m_slots.remove_all_matching([&](GC::Weak<HTML::HTMLSlotElement>& s) {
        return !s || &slot == s.ptr();
    });
}

GC::Ptr<HTML::HTMLSlotElement> SlotRegistry::first_slot_with_name(StringView name) const
{
    for (auto const& slot : m_slots) {
        if (slot && slot->slot_name() == name)
            return slot.ptr();
    }

    return nullptr;
}

bool SlotRegistry::is_empty() const
{
    return m_slot_count == 0;
}

}
