/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>

#pragma once

namespace GC {

template<typename T>
class HeapVector : public Cell {
    GC_CELL(HeapVector, Cell);

public:
    HeapVector() = default;
    virtual ~HeapVector() override = default;

    auto& elements() { return m_elements; }
    auto const& elements() const { return m_elements; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_elements);
    }

private:
    Vector<T> m_elements;
};

}
