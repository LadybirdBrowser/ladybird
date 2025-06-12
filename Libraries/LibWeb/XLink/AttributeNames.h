/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::XLink::AttributeNames {

#define ENUMERATE_XLINK_ATTRIBUTES       \
    __ENUMERATE_XLINK_ATTRIBUTE(actuate) \
    __ENUMERATE_XLINK_ATTRIBUTE(arcrole) \
    __ENUMERATE_XLINK_ATTRIBUTE(from)    \
    __ENUMERATE_XLINK_ATTRIBUTE(href)    \
    __ENUMERATE_XLINK_ATTRIBUTE(label)   \
    __ENUMERATE_XLINK_ATTRIBUTE(role)    \
    __ENUMERATE_XLINK_ATTRIBUTE(show)    \
    __ENUMERATE_XLINK_ATTRIBUTE(title)   \
    __ENUMERATE_XLINK_ATTRIBUTE(to)      \
    __ENUMERATE_XLINK_ATTRIBUTE(type)

#define __ENUMERATE_XLINK_ATTRIBUTE(name) extern FlyString name;
ENUMERATE_XLINK_ATTRIBUTES
#undef __ENUMERATE_XLINK_ATTRIBUTE

}
