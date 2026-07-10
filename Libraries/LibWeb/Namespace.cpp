/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Namespace.h>

namespace Web::Namespace {

#define __ENUMERATE_NAMESPACE(name, namespace_) \
    Utf16FlyString const& name = *new Utf16FlyString(namespace_##_utf16_fly_string);
ENUMERATE_NAMESPACES
#undef __ENUMERATE_NAMESPACE

}
