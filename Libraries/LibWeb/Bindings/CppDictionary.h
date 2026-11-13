/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>

namespace Web::Bindings {

// A dictionary that the C++ bindings converted for an interface implemented in Rust, which hands it to the C++ code
// that uses it. Bindings/Rust/src/bindings/conversions.rs holds it as a CppDictionary.
template<typename Dictionary>
class CppDictionary final : public GC::Cell {
    GC_CELL(CppDictionary, GC::Cell);
    GC_DECLARE_ALLOCATOR(CppDictionary);

public:
    explicit CppDictionary(Dictionary dictionary)
        : m_dictionary(move(dictionary))
    {
    }

    Dictionary const& dictionary() const { return m_dictionary; }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        m_dictionary.visit_edges(visitor);
    }

    Dictionary m_dictionary;
};

template<typename Dictionary>
GC_DEFINE_ALLOCATOR(CppDictionary<Dictionary>);

}
