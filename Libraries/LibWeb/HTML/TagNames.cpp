/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/TagNames.h>

namespace Web::HTML::TagNames {

#define __ENUMERATE_HTML_TAG(name, tag) \
    FlyString name = tag##_fly_string;
ENUMERATE_HTML_TAGS
#undef __ENUMERATE_HTML_TAG

}
