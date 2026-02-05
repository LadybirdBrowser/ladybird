/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PseudoClass.h>

namespace Web::CSS {

class PseudoClassBitmap {
public:
    PseudoClassBitmap() = default;
    ~PseudoClassBitmap() = default;

    void set(PseudoClass pseudo_class, bool bit)
    {
        size_t const index = to_underlying(pseudo_class);
        if (bit)
            m_bits |= 1LLU << index;
        else
            m_bits &= ~(1LLU << index);
    }

    bool get(PseudoClass pseudo_class) const
    {
        size_t const index = to_underlying(pseudo_class);
        return (m_bits & (1LLU << index)) != 0;
    }

    void operator|=(PseudoClassBitmap const& other)
    {
        m_bits |= other.m_bits;
    }

private:
    u64 m_bits { 0 };
};

// NOTE: If this changes, we'll have to tweak PseudoClassBitmap a little bit :)
static_assert(to_underlying(PseudoClass::__Count) <= 64);

}
