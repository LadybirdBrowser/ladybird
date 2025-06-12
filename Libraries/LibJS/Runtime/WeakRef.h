/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/WeakContainer.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class WeakRef final
    : public Object
    , public GC::WeakContainer {
    JS_OBJECT(WeakRef, Object);
    GC_DECLARE_ALLOCATOR(WeakRef);

public:
    static GC::Ref<WeakRef> create(Realm&, Object&);
    static GC::Ref<WeakRef> create(Realm&, Symbol&);

    virtual ~WeakRef() override = default;

    auto const& value() const { return m_value; }

    void update_execution_generation() { m_last_execution_generation = vm().execution_generation(); }

    virtual void remove_dead_cells(Badge<GC::Heap>) override;

private:
    explicit WeakRef(Object&, Object& prototype);
    explicit WeakRef(Symbol&, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    Variant<GC::Ptr<Object>, GC::Ptr<Symbol>, Empty> m_value;
    u32 m_last_execution_generation { 0 };
};

}
