/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/SafeFunction.h>
#include <LibWeb/Forward.h>

namespace Web::Platform {

class EventLoopPlugin {
public:
    static EventLoopPlugin& the();
    static void install(EventLoopPlugin&);

    virtual ~EventLoopPlugin();

    virtual void spin_until(GC::SafeFunction<bool()> goal_condition) = 0;
    virtual void deferred_invoke(ESCAPING GC::SafeFunction<void()>) = 0;
    virtual NonnullRefPtr<Timer> create_timer() = 0;
    virtual void quit() = 0;
};

}
