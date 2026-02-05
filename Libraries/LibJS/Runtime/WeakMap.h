/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/WeakContainer.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API WeakMap final
    : public Object
    , public GC::WeakContainer {
    JS_OBJECT(WeakMap, Object);
    GC_DECLARE_ALLOCATOR(WeakMap);

public:
    static GC::Ref<WeakMap> create(Realm&);

    virtual ~WeakMap() override = default;

    HashMap<GC::Ptr<Cell>, Value> const& values() const { return m_values; }
    HashMap<GC::Ptr<Cell>, Value>& values() { return m_values; }

    virtual void remove_dead_cells(Badge<GC::Heap>) override;

private:
    explicit WeakMap(Object& prototype);

    virtual bool is_weak_map() const final { return true; }
    void visit_edges(Visitor&) override;

    HashMap<GC::Ptr<Cell>, Value> m_values; // This stores Cell pointers instead of Object pointers to aide with sweeping
};

template<>
inline bool Object::fast_is<WeakMap>() const { return is_weak_map(); }

}
