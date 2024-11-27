/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Editing/CommandNames.h>

namespace Web::Editing::CommandNames {

#define __ENUMERATE_COMMAND_NAME(name) FlyString name;
ENUMERATE_COMMAND_NAMES
#undef __ENUMERATE_COMMAND_NAME
FlyString delete_;

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_COMMAND_NAME(name) name = #name##_fly_string;
    ENUMERATE_COMMAND_NAMES
#undef __ENUMERATE_MATHML_TAG
    delete_ = "delete"_fly_string;

    s_initialized = true;
}

}
