/*
 * Copyright (c) 2020-2024, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>

namespace JS {

class Heap : public GC::Heap {
public:
    explicit Heap(VM& vm, AK::Function<void(HashMap<GC::Cell*, GC::HeapRoot>&)> gather_roots)
        : GC::Heap(&vm, move(gather_roots))
    {
    }

    template<typename T, typename... Args>
    GC::Ref<T> allocate(Realm& realm, Args&&... args)
    {
        auto* memory = allocate_cell<T>();
        defer_gc();
        new (memory) T(forward<Args>(args)...);
        undefer_gc();
        auto* cell = static_cast<T*>(memory);
        static_cast<Cell*>(cell)->initialize(realm);
        return *cell;
    }

    template<typename T, typename... Args>
    GC::Ref<T> allocate_without_realm(Args&&... args)
    {
        auto* memory = allocate_cell<T>();
        defer_gc();
        new (memory) T(forward<Args>(args)...);
        undefer_gc();
        return *static_cast<T*>(memory);
    }
};

}
