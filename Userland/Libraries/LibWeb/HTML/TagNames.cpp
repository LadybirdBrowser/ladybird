/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/TagNames.h>

namespace Web::HTML::TagNames {

#define __ENUMERATE_HTML_TAG(name) FlyString name;
ENUMERATE_HTML_TAGS
#undef __ENUMERATE_HTML_TAG

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_HTML_TAG(name) \
    name = #name##_fly_string;
    ENUMERATE_HTML_TAGS
#undef __ENUMERATE_HTML_TAG

    template_ = "template"_fly_string;

    s_initialized = true;
}

}
