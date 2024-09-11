/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ScrollFrame : public RefCounted<ScrollFrame> {
public:
    ScrollFrame(i32 id, RefPtr<ScrollFrame const> parent)
        : m_id(id)
        , m_parent(move(parent))
    {
    }

    i32 id() const { return m_id; }

    CSSPixelPoint cumulative_offset() const
    {
        if (m_parent)
            return m_parent->cumulative_offset() + m_own_offset;
        return m_own_offset;
    }

    CSSPixelPoint own_offset() const { return m_own_offset; }
    void set_own_offset(CSSPixelPoint offset) { m_own_offset = offset; }

private:
    i32 m_id { -1 };
    RefPtr<ScrollFrame const> m_parent;
    CSSPixelPoint m_own_offset;
};

}
