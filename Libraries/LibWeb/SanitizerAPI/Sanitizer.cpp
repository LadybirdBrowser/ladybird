/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SanitizerAPI/Sanitizer.h>

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::SanitizerAPI {

GC_DEFINE_ALLOCATOR(Sanitizer);

Sanitizer::Sanitizer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Sanitizer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Sanitizer);
    Base::initialize(realm);
}

}
