/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/EventNames.h>

namespace Web::HTML::EventNames {

#define __ENUMERATE_HTML_EVENT(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_HTML_EVENTS
#undef __ENUMERATE_HTML_EVENT

}
