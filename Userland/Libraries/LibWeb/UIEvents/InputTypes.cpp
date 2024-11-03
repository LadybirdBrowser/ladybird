/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::UIEvents::InputTypes {

#define __ENUMERATE_INPUT_TYPE(name) FlyString name;
ENUMERATE_INPUT_TYPES
#undef __ENUMERATE_INPUT_TYPE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_INPUT_TYPE(name) \
    name = #name##_fly_string;
    ENUMERATE_INPUT_TYPES
#undef __ENUMERATE_INPUT_TYPE

    s_initialized = true;
}

}
