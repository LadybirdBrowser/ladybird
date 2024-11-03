/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Platform {

EventLoopPlugin* s_the;

EventLoopPlugin& EventLoopPlugin::the()
{
    VERIFY(s_the);
    return *s_the;
}

void EventLoopPlugin::install(EventLoopPlugin& plugin)
{
    VERIFY(!s_the);
    s_the = &plugin;
}

EventLoopPlugin::~EventLoopPlugin() = default;

}
