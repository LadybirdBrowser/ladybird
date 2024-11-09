/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ValidityStatePrototype.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ValidityState);

ValidityState::ValidityState(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void ValidityState::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ValidityState);
}

}
