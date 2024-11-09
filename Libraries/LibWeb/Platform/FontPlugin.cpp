/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Platform {

static FontPlugin* s_the;

FontPlugin& FontPlugin::the()
{
    VERIFY(s_the);
    return *s_the;
}

void FontPlugin::install(FontPlugin& plugin)
{
    VERIFY(!s_the);
    s_the = &plugin;
}

FontPlugin::~FontPlugin() = default;

}
