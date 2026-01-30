/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Platform/FontPlugin.h>

#ifndef LIBWEB_UNITY_ID
#    define LIBWEB_UNITY_ID LIBWEB_UNITY_ID_FALLBACK
#endif

namespace Web::Platform {

namespace {
namespace LIBWEB_UNITY_ID {

static FontPlugin* s_the;

}
}

FontPlugin& FontPlugin::the()
{
    VERIFY(LIBWEB_UNITY_ID::s_the);
    return *LIBWEB_UNITY_ID::s_the;
}

void FontPlugin::install(FontPlugin& plugin)
{
    VERIFY(!LIBWEB_UNITY_ID::s_the);
    LIBWEB_UNITY_ID::s_the = &plugin;
}

FontPlugin::~FontPlugin() = default;

}
