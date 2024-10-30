/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Forward.h>

namespace Web::Platform {

class EventLoopPlugin {
public:
    static EventLoopPlugin& the();
    static void install(EventLoopPlugin&);

    virtual ~EventLoopPlugin();

    virtual void spin_until(JS::Handle<JS::HeapFunction<bool()>> goal_condition) = 0;
    virtual void deferred_invoke(ESCAPING JS::Handle<JS::HeapFunction<void()>>) = 0;
    virtual JS::NonnullGCPtr<Timer> create_timer(JS::Heap&) = 0;
    virtual void quit() = 0;
};

}
