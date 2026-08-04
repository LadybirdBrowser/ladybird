/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/SourceLocation.h>
#include <LibGC/Cell.h>
#include <LibGC/Root.h>

namespace GC {

class ActivityRoot {
    AK_MAKE_NONCOPYABLE(ActivityRoot);
    AK_MAKE_NONMOVABLE(ActivityRoot);

public:
    ActivityRoot() = default;
    ~ActivityRoot() { release(); }

    // ActivityRoot stores a normal GC root. The rooted cell must be the object
    // that owns this ActivityRoot; rooting an unrelated cell can leave the root
    // destructor touching freed storage if the owner dies first.
    void take(Cell& cell, SourceLocation location = SourceLocation::current())
    {
        if (m_root.ptr() == &cell)
            return;
        m_root = Root<Cell>::create(&cell, location);
    }

    void release()
    {
        m_root = {};
    }

    bool is_taken() const { return !!m_root; }
    Cell* cell() const { return m_root.ptr(); }

private:
    Root<Cell> m_root;
};

}
