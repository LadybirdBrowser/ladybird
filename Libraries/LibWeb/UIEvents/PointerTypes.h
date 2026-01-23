/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Export.h>

namespace Web::UIEvents::PointerTypes {

// https://w3c.github.io/pointerevents/#dom-pointerevent-pointertype
#define ENUMERATE_POINTER_TYPES              \
    __ENUMERATE_POINTER_TYPE(Mouse, "mouse") \
    __ENUMERATE_POINTER_TYPE(Pen, "pen")     \
    __ENUMERATE_POINTER_TYPE(Touch, "touch")

#define __ENUMERATE_POINTER_TYPE(name, value) extern WEB_API String name;
ENUMERATE_POINTER_TYPES
#undef __ENUMERATE_POINTER_TYPE

}
