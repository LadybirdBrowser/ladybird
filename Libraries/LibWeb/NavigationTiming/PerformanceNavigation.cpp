/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/NavigationTiming/PerformanceNavigation.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceNavigation);

GC::Ref<PerformanceNavigation> PerformanceNavigation::create(u16 type, u16 redirect_count)
{
    return GC::Heap::the().allocate<PerformanceNavigation>(type, redirect_count);
}

PerformanceNavigation::PerformanceNavigation(u16 type, u16 redirect_count)
    : m_type(type)
    , m_redirect_count(redirect_count)
{
}
PerformanceNavigation::~PerformanceNavigation() = default;

u16 PerformanceNavigation::type() const
{
    return m_type;
}

u16 PerformanceNavigation::redirect_count() const
{
    return m_redirect_count;
}

}
