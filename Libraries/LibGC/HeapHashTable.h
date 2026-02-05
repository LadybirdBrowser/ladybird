/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>

#pragma once

namespace GC {

template<typename T>
class HeapHashTable : public Cell {
    GC_CELL(HeapHashTable, Cell);
    GC_DECLARE_ALLOCATOR(HeapHashTable);

public:
    HeapHashTable() = default;
    virtual ~HeapHashTable() override = default;

    auto& table() { return m_table; }
    auto const& table() const { return m_table; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_table);
    }

private:
    HashTable<T> m_table;
};

template<typename T>
GC_DEFINE_ALLOCATOR(HeapHashTable<T>);

}
