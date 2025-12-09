/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>

namespace JS {

class JS_API Cell : public GC::Cell {
    GC_CELL(Cell, GC::Cell);

public:
    virtual void initialize(Realm&);

    virtual bool is_generator_result() const { return false; }

    ALWAYS_INLINE VM& vm() const;
};

}
