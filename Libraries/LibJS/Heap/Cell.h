/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>

namespace JS {

class JS_API Cell : public GC::Cell {
    GC_CELL(Cell, GC::Cell);

public:
    MUST_UPCALL virtual void initialize(Realm&);

    virtual bool is_generator_result() const { return false; }
    virtual bool is_environment() const { return false; }

    ALWAYS_INLINE VM& vm() const;

    template<typename T>
    bool fast_is() const = delete;
};

}
