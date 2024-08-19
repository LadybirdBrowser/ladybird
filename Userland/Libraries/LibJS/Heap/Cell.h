/*
 * Copyright (c) 2020-2024, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>

namespace JS {

#define JS_CELL(class_, base_class)                \
public:                                            \
    using Base = base_class;                       \
    virtual StringView class_name() const override \
    {                                              \
        return #class_##sv;                        \
    }                                              \
    friend class JS::Heap;

class Cell : public GC::Cell {
public:
    virtual ~Cell() = default;
    virtual void initialize(Realm&);

    ALWAYS_INLINE Heap& heap() const { return reinterpret_cast<Heap&>(GC::HeapBlockBase::from_cell(this)->heap()); }
    ALWAYS_INLINE VM& vm() const { return *static_cast<VM*>(private_data()); }
};

}
