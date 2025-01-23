/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchTimingInfo);

FetchTimingInfo::FetchTimingInfo() = default;

GC::Ref<FetchTimingInfo> FetchTimingInfo::create(JS::VM& vm)
{
    return vm.heap().allocate<FetchTimingInfo>();
}

void FetchTimingInfo::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://fetch.spec.whatwg.org/#create-an-opaque-timing-info
GC::Ref<FetchTimingInfo> create_opaque_timing_info(JS::VM& vm, FetchTimingInfo const& timing_info)
{
    // To create an opaque timing info, given a fetch timing info timingInfo, return a new fetch timing info whose
    // start time and post-redirect start time are timingInfo’s start time.
    auto new_timing_info = FetchTimingInfo::create(vm);
    new_timing_info->set_start_time(timing_info.start_time());
    new_timing_info->set_post_redirect_start_time(timing_info.start_time());
    return new_timing_info;
}

}
