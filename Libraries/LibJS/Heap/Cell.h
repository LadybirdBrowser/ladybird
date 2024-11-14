/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/Forward.h>

namespace JS {

class Cell : public GC::Cell {
    GC_CELL(Cell, GC::Cell);

public:
    virtual void initialize(Realm&);

    ALWAYS_INLINE VM& vm() const { return *reinterpret_cast<VM*>(private_data()); }
};

}
