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

    if (m_slots.is_empty() || m_slots.last()->is_before(slot) || !try_insert_in_tree_order(slot)) {
        m_slots.empend(slot);
        ++m_slot_count;
    }
}

bool SlotRegistry::try_insert_in_tree_order(HTML::HTMLSlotElement& slot)
{
    // Walk forward from the slot we're adding to find the next registered slot. If we find one, insert before it.
    auto& shadow_root = slot.root();
    for (auto* node = slot.next_in_pre_order(&shadow_root); node; node = node->next_in_pre_order(&shadow_root)) {
        auto* following_slot = as_if<HTML::HTMLSlotElement>(*node);
        if (!following_slot)
            continue;

        auto index = m_slots.find_first_index_if([&](auto const& s) {
            return s.ptr() == following_slot;
        });

        if (index.has_value()) {
            m_slots.insert(index.value(), GC::Weak { slot });
            ++m_slot_count;
            return true;
        }
    }
    return false;
}

void SlotRegistry::remove(HTML::HTMLSlotElement& slot)
{
    m_slot_count -= m_slots.remove_all_matching([&](GC::Weak<HTML::HTMLSlotElement>& s) {
        return !s || &slot == s.ptr();
    });
}

GC::Ptr<HTML::HTMLSlotElement> SlotRegistry::first_slot_with_name(FlyString const& name) const
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
