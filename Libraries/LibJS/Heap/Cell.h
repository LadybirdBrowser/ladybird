/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/CellImpl.h>

namespace JS {

class Cell : public CellImpl {
    JS_CELL(Cell, CellImpl);

public:
    virtual void initialize(Realm&);

    ALWAYS_INLINE VM& vm() const { return *reinterpret_cast<VM*>(private_data()); }
};

}
