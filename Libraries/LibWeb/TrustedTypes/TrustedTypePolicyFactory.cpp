/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicyFactory);

GC::Ref<TrustedTypePolicyFactory> TrustedTypePolicyFactory::create(JS::Realm& realm)
{
    return realm.create<TrustedTypePolicyFactory>(realm);
}

TrustedTypePolicyFactory::TrustedTypePolicyFactory(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void TrustedTypePolicyFactory::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicyFactory);
    Base::initialize(realm);
}

}
