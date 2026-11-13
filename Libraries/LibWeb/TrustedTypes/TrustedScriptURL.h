/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TrustedScriptURL.h>

namespace Web::TrustedTypes {

using TrustedScriptURLOrString = Variant<GC::Ref<TrustedScriptURL>, Utf16String>;

class TrustedScriptURL final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TrustedScriptURL, trusted_script_url)

public:
    [[nodiscard]] static GC::Ref<TrustedScriptURL> create(Utf16String data);

    Utf16String to_string() const;
};

}
