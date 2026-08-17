/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class Accessor final : public Cell {
    GC_CELL(Accessor, Cell);
    GC_DECLARE_ALLOCATOR(Accessor);

public:
    static GC::Ref<Accessor> create(VM& vm, GC::Ptr<FunctionObject> getter, GC::Ptr<FunctionObject> setter, GC::Ptr<Symbol> cached_value_key = nullptr)
    {
        return vm.heap().allocate<Accessor>(getter, setter, cached_value_key);
    }

    FunctionObject* getter() const { return m_getter.ptr(); }
    void set_getter(GC::Ptr<FunctionObject> getter) { m_getter = getter; }

    FunctionObject* setter() const { return m_setter.ptr(); }
    void set_setter(GC::Ptr<FunctionObject> setter) { m_setter = setter; }

    Symbol* cached_value_key() const { return m_cached_value_key.ptr(); }
    void set_cached_value_key(GC::Ptr<Symbol> cached_value_key) { m_cached_value_key = cached_value_key; }

    void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_getter);
        visitor.visit(m_setter);
        visitor.visit(m_cached_value_key);
    }

private:
    Accessor(GC::Ptr<FunctionObject> getter, GC::Ptr<FunctionObject> setter, GC::Ptr<Symbol> cached_value_key)
        : m_getter(getter)
        , m_setter(setter)
        , m_cached_value_key(cached_value_key)
    {
    }

    GC::Ptr<FunctionObject> m_getter;
    GC::Ptr<FunctionObject> m_setter;
    // Cached accessor values live in private properties on the holder object.
    GC::Ptr<Symbol> m_cached_value_key;
};

}
