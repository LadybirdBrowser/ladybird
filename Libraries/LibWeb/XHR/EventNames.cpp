/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/XHR/EventNames.h>

namespace Web::XHR::EventNames {

#define __ENUMERATE_XHR_EVENT(name) \
    FlyString const& name = *new FlyString(#name##_fly_string);
ENUMERATE_XHR_EVENTS
#undef __ENUMERATE_XHR_EVENT

}
