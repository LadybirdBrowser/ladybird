/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <LibCore/Export.h>

namespace Core {

#ifdef AK_OS_MACOS

class CORE_API ScopedAutoreleasePool {
    AK_MAKE_NONCOPYABLE(ScopedAutoreleasePool);
    AK_MAKE_NONMOVABLE(ScopedAutoreleasePool);

public:
    ScopedAutoreleasePool();
    ~ScopedAutoreleasePool();

private:
    void* m_pool;
};

#else

class ScopedAutoreleasePool {
public:
    ScopedAutoreleasePool() = default;
    ~ScopedAutoreleasePool() { }
};

#endif

}
