/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::TrustedTypes {

using TrustedHTMLOrString = Variant<GC::Ref<TrustedHTML>, Utf16String>;

class TrustedHTML final : public Bindings::Wrappable {
    WEB_WRAPPABLE(TrustedHTML, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TrustedHTML);

public:
    virtual ~TrustedHTML() override = default;

    Utf16String const& to_string() const;
    Utf16String const& to_json() const;

private:
    explicit TrustedHTML(Utf16String);

    Utf16String const m_data;
};

}
