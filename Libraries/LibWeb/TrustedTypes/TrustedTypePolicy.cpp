/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicy);

TrustedTypePolicy::TrustedTypePolicy(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void TrustedTypePolicy::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicy);
    Base::initialize(realm);
}

}
