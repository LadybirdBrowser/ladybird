/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedScriptURL.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedScriptURL);

GC::Ref<TrustedScriptURL> TrustedScriptURL::create(JS::Realm& realm, String const& data)
{
    return realm.create<TrustedScriptURL>(realm, data);
}

TrustedScriptURL::TrustedScriptURL(JS::Realm& realm, String const& data)
    : PlatformObject(realm)
    , m_data(data)
{
}

void TrustedScriptURL::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedScriptURL);
    Base::initialize(realm);
}

String TrustedScriptURL::to_string() const
{
    if (m_data.has_value())
        return m_data.value();
    return ""_string;
}

String TrustedScriptURL::to_json() const
{
    return to_string();
}

}
