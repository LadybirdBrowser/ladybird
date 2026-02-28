/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Function.h>
#include <LibWeb/Export.h>

namespace Web::Platform {

class WEB_API EventLoopPlugin {
public:
    static EventLoopPlugin& the();
    static void install(EventLoopPlugin&);

    ~EventLoopPlugin();

    void spin_until(GC::Root<GC::Function<bool()>> goal_condition);
    void deferred_invoke(ESCAPING GC::Root<GC::Function<void()>>);
    void quit();
};

}
