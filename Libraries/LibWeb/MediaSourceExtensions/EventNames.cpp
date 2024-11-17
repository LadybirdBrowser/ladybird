/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MediaSourceExtensions/EventNames.h>

namespace Web::MediaSourceExtensions::EventNames {

#define __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(name) FlyString name;
ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES(__ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE)
#undef __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(name) \
    name = #name##_fly_string;
    ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES(__ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE)
#undef __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE

    s_initialized = true;
}

}
