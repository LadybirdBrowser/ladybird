/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-5/#random-caching
// The random base values of the keys that name an element, which the style engine keeps for the element in a slot the
// element holds while it has no style node. The element's next style node takes them back; an element that never gets
// one lets go of them with the slot.
class ParkedRandomBaseValues {
    AK_MAKE_NONCOPYABLE(ParkedRandomBaseValues);

public:
    ParkedRandomBaseValues() = default;
    explicit ParkedRandomBaseValues(void const* slot)
        : m_slot(slot)
    {
    }
    ParkedRandomBaseValues(ParkedRandomBaseValues&& other)
        : m_slot(exchange(other.m_slot, nullptr))
    {
    }
    ParkedRandomBaseValues& operator=(ParkedRandomBaseValues&& other)
    {
        swap(m_slot, other.m_slot);
        return *this;
    }
    ~ParkedRandomBaseValues();

    explicit operator bool() const { return m_slot; }
    [[nodiscard]] void const* leak_slot() { return exchange(m_slot, nullptr); }

private:
    void const* m_slot { nullptr };
};

}
