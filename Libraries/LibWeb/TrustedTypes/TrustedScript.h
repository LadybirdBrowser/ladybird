/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TrustedScript.h>

namespace Web::TrustedTypes {

using TrustedScriptOrString = Variant<GC::Ref<TrustedScript>, Utf16String>;
using NullableTrustedScriptOrString = Variant<GC::Ref<TrustedScript>, Utf16String, Empty>;

class TrustedScript final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TrustedScript, trusted_script)

public:
    [[nodiscard]] static GC::Ref<TrustedScript> create(Utf16String data);

    Utf16String to_string() const;
};

}
