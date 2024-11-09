/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PerformanceTimingPrototype.h>
#include <LibWeb/NavigationTiming/PerformanceTiming.h>

namespace Web::NavigationTiming {

JS_DEFINE_ALLOCATOR(PerformanceTiming);

PerformanceTiming::PerformanceTiming(JS::Realm& realm)
    : PlatformObject(realm)
{
}

PerformanceTiming::~PerformanceTiming() = default;

void PerformanceTiming::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceTiming);
}

}
