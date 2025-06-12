/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Namespace.h>

namespace Web::Namespace {

#define __ENUMERATE_NAMESPACE(name, namespace_) \
    FlyString name = namespace_##_fly_string;
ENUMERATE_NAMESPACES
#undef __ENUMERATE_NAMESPACE

}
