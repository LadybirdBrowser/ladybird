/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibGC/Root.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web {

enum class ScreenWakeLockState {
    Released,
    Acquired,
};

class ScreenWakeLockHandle {
    AK_MAKE_NONCOPYABLE(ScreenWakeLockHandle);
    AK_MAKE_NONMOVABLE(ScreenWakeLockHandle);

public:
    explicit ScreenWakeLockHandle(Page&);
    ~ScreenWakeLockHandle();

    void visit_edges(JS::Cell::Visitor&);

private:
    GC::Ref<Page> m_page;
};

}
