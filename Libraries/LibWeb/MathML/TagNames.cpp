/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML::TagNames {

#define __ENUMERATE_MATHML_TAG(name) FlyString name;
ENUMERATE_MATHML_TAGS
#undef __ENUMERATE_MATHML_TAG
FlyString annotation_xml;

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_MATHML_TAG(name) name = #name##_fly_string;
    ENUMERATE_MATHML_TAGS
#undef __ENUMERATE_MATHML_TAG
    annotation_xml = "annotation-xml"_fly_string;

    s_initialized = true;
}

}
