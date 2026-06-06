/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedScriptURL.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedScriptURL);

TrustedScriptURL::TrustedScriptURL(JS::Realm& realm, Utf16String data)
    : Wrappable(realm)
    , m_data(move(data))
{
}

// https://w3c.github.io/trusted-types/dist/spec/#trustedscripturl-stringification-behavior
Utf16String const& TrustedScriptURL::to_string() const
{
    // 1. return the associated data value.
    return m_data;
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedscripturl-tojson
Utf16String const& TrustedScriptURL::to_json() const
{
    // 1. return the associated data value.
    return to_string();
}

}
