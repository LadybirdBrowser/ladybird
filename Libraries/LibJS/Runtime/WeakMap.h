/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/WeakContainer.h>

namespace JS {

class WeakMap final
    : public Object
    , public WeakContainer {
    JS_OBJECT(WeakMap, Object);
    JS_DECLARE_ALLOCATOR(WeakMap);

public:
    static NonnullGCPtr<WeakMap> create(Realm&);

    virtual ~WeakMap() override = default;

    HashMap<GCPtr<Cell>, Value> const& values() const { return m_values; }
    HashMap<GCPtr<Cell>, Value>& values() { return m_values; }

    virtual void remove_dead_cells(Badge<Heap>) override;

private:
    explicit WeakMap(Object& prototype);

    void visit_edges(Visitor&) override;

    HashMap<GCPtr<Cell>, Value> m_values; // This stores Cell pointers instead of Object pointers to aide with sweeping
};

}
