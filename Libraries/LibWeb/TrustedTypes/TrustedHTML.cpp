/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedHTML.h>

#include <LibGC/Ptr.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedHTML);

TrustedHTML::TrustedHTML(Utf16String data)
    : Bindings::Wrappable()
    , m_data(move(data))
{
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
