/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Platform/ScopedAutoreleasePool.h>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace Core {

ScopedAutoreleasePool::ScopedAutoreleasePool()
    : m_pool(objc_autoreleasePoolPush())
{
}

ScopedAutoreleasePool::~ScopedAutoreleasePool()
{
    objc_autoreleasePoolPop(m_pool);
}

}
