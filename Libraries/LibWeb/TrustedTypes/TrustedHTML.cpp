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

TrustedHTML::TrustedHTML(JS::Realm& realm, Utf16String data)
    : PlatformObject(realm)
    , m_data(move(data))
{
}

void TrustedHTML::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedHTML);
    Base::initialize(realm);
}

// https://w3c.github.io/trusted-types/dist/spec/#trustedhtml-stringification-behavior
Utf16String const& TrustedHTML::to_string() const
{
    // 1. return the associated data value.
    return m_data;
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedhtml-tojson
Utf16String const& TrustedHTML::to_json() const
{
    // 1. return the associated data value.
    return to_string();
}

}
