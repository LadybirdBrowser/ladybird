/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/XHR/EventNames.h>

namespace Web::XHR::EventNames {

#define __ENUMERATE_XHR_EVENT(name) \
    Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_XHR_EVENTS
#undef __ENUMERATE_XHR_EVENT

}
