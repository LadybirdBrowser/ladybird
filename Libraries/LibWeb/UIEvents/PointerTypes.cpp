/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/UIEvents/PointerTypes.h>

namespace Web::UIEvents::PointerTypes {

#define __ENUMERATE_POINTER_TYPE(name, value) \
    Utf16FlyString name = value##_utf16_fly_string;
ENUMERATE_POINTER_TYPES
#undef __ENUMERATE_POINTER_TYPE

}
