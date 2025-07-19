/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class Accessor final : public Cell {
    GC_CELL(Accessor, Cell);
    GC_DECLARE_ALLOCATOR(Accessor);

public:
    static GC::Ref<Accessor> create(VM& vm, FunctionObject* getter, FunctionObject* setter)
    {
        return vm.heap().allocate<Accessor>(getter, setter);
    }

    FunctionObject* getter() const { return m_getter; }
    void set_getter(FunctionObject* getter) { m_getter = getter; }

    FunctionObject* setter() const { return m_setter; }
    void set_setter(FunctionObject* setter) { m_setter = setter; }

    void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_getter);
        visitor.visit(m_setter);
    }

private:
    Accessor(FunctionObject* getter, FunctionObject* setter)
        : m_getter(getter)
        , m_setter(setter)
    {
    }

    GC::Ptr<FunctionObject> m_getter;
    GC::Ptr<FunctionObject> m_setter;
};

}
