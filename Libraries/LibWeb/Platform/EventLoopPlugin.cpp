/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

#ifndef LIBWEB_UNITY_ID
#    define LIBWEB_UNITY_ID LIBWEB_UNITY_ID_FALLBACK
#endif

namespace Web::Platform {

namespace {
namespace LIBWEB_UNITY_ID {

EventLoopPlugin* s_the;

}
}

EventLoopPlugin& EventLoopPlugin::the()
{
    VERIFY(LIBWEB_UNITY_ID::s_the);
    return *LIBWEB_UNITY_ID::s_the;
}

void EventLoopPlugin::install(EventLoopPlugin& plugin)
{
    VERIFY(!LIBWEB_UNITY_ID::s_the);
    LIBWEB_UNITY_ID::s_the = &plugin;
}

EventLoopPlugin::~EventLoopPlugin() = default;

}
