/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PerformanceTimingPrototype.h>
#include <LibWeb/NavigationTiming/PerformanceTiming.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceTiming);

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

DOM::DocumentLoadTimingInfo const& PerformanceTiming::document_load_timing_info() const
{
    auto& global_object = HTML::relevant_global_object(*this);
    VERIFY(is<HTML::Window>(global_object));
    auto& window = static_cast<HTML::Window&>(global_object);
    auto document = window.document();
    return document->load_timing_info();
}

}
