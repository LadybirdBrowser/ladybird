/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Symbol.h>

namespace JS {

class SymbolObject : public Object {
    JS_OBJECT(SymbolObject, Object);
    JS_DECLARE_ALLOCATOR(SymbolObject);

public:
    static NonnullGCPtr<SymbolObject> create(Realm&, Symbol&);

    virtual ~SymbolObject() override = default;

    Symbol& primitive_symbol() { return m_symbol; }
    Symbol const& primitive_symbol() const { return m_symbol; }

private:
    SymbolObject(Symbol&, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    NonnullGCPtr<Symbol> m_symbol;
};

}
