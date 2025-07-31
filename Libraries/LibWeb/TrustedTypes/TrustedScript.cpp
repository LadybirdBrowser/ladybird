/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedScript.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedScript);

GC::Ref<TrustedScript> TrustedScript::create(JS::Realm& realm, String const& data)
{
    return realm.create<TrustedScript>(realm, data);
}

TrustedScript::TrustedScript(JS::Realm& realm, String const& data)
    : PlatformObject(realm)
    , m_data(data)
{
}

void TrustedScript::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedScript);
    Base::initialize(realm);
}

String TrustedScript::to_string() const
{
    if (m_data.has_value())
        return m_data.value();
    return ""_string;
}

String TrustedScript::to_json() const
{
    return to_string();
}

}
