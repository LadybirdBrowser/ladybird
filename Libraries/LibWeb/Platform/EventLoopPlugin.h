/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::Platform {

class EventLoopPlugin {
public:
    static EventLoopPlugin& the();
    static void install(EventLoopPlugin&);

    virtual ~EventLoopPlugin();

    virtual void spin_until(GC::Root<GC::Function<bool()>> goal_condition) = 0;
    virtual void deferred_invoke(ESCAPING GC::Root<GC::Function<void()>>) = 0;
    virtual GC::Ref<Timer> create_timer(GC::Heap&) = 0;
    virtual void quit() = 0;
};

}
