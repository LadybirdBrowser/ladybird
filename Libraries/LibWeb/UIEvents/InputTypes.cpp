/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::UIEvents::InputTypes {

#define __ENUMERATE_INPUT_TYPE(name) \
    Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_INPUT_TYPES
#undef __ENUMERATE_INPUT_TYPE

}
