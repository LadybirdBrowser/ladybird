/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceNavigationPrototype.h>
#include <LibWeb/NavigationTiming/PerformanceNavigation.h>

namespace Web::NavigationTiming {

JS_DEFINE_ALLOCATOR(PerformanceNavigation);

PerformanceNavigation::PerformanceNavigation(JS::Realm& realm, u16 type, u16 redirect_count)
    : PlatformObject(realm)
    , m_type(type)
    , m_redirect_count(redirect_count)
{
}
PerformanceNavigation::~PerformanceNavigation() = default;

void PerformanceNavigation::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceNavigation);
}

u16 PerformanceNavigation::type() const
{
    return m_type;
}

u16 PerformanceNavigation::redirect_count() const
{
    return m_redirect_count;
}

}
