/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedHTML.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedHTML);

GC::Ref<TrustedHTML> TrustedHTML::create(JS::Realm& realm, String const& data)
{
    return realm.create<TrustedHTML>(realm, data);
}

TrustedHTML::TrustedHTML(JS::Realm& realm, String const& data)
    : PlatformObject(realm)
    , m_data(data)
{
}

void TrustedHTML::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedHTML);
    Base::initialize(realm);
}

String TrustedHTML::to_string() const
{
    return m_data;
}

String TrustedHTML::to_json() const
{
    return m_data;
}

}
