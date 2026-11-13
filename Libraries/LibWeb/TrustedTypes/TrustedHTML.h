/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TrustedHTML.h>

namespace Web::TrustedTypes {

using TrustedHTMLOrString = Variant<GC::Ref<TrustedHTML>, Utf16String>;

class TrustedHTML final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TrustedHTML, trusted_html)

public:
    [[nodiscard]] static GC::Ref<TrustedHTML> create(Utf16String data);

    Utf16String to_string() const;
};

}
