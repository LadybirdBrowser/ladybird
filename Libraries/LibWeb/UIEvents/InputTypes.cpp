/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::UIEvents::InputTypes {

#define __ENUMERATE_INPUT_TYPE(name) \
    FlyString const& name = *new FlyString(#name##_fly_string);
ENUMERATE_INPUT_TYPES
#undef __ENUMERATE_INPUT_TYPE

}
