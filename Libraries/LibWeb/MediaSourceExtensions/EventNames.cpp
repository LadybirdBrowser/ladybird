/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MediaSourceExtensions/EventNames.h>

namespace Web::MediaSourceExtensions::EventNames {

#define __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTES
#undef __ENUMERATE_MEDIA_SOURCE_EXTENSIONS_ATTRIBUTE

}
