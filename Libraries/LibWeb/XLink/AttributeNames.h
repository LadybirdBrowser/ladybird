/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FlyString.h>

namespace Web::XLink::AttributeNames {

#define ENUMERATE_XLINK_ATTRIBUTES(E) \
    E(type)                           \
    E(href)                           \
    E(role)                           \
    E(arcrole)                        \
    E(title)                          \
    E(show)                           \
    E(actuate)                        \
    E(label)                          \
    E(from)                           \
    E(to)

#define __ENUMERATE_XLINK_ATTRIBUTE(name) extern FlyString name;
ENUMERATE_XLINK_ATTRIBUTES(__ENUMERATE_XLINK_ATTRIBUTE)
#undef __ENUMERATE_XLINK_ATTRIBUTE

void initialize_strings();

}
