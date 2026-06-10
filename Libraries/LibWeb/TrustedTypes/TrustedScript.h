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

using TrustedScriptOrString = Variant<GC::Ref<TrustedScript>, Utf16String>;
using NullableTrustedScriptOrString = Variant<GC::Ref<TrustedScript>, Utf16String, Empty>;

class TrustedScript final : public Bindings::Wrappable {
    WEB_WRAPPABLE(TrustedScript, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TrustedScript);

public:
    virtual ~TrustedScript() override = default;

    Utf16String const& to_string() const;
    Utf16String const& to_json() const;

private:
    explicit TrustedScript(Utf16String);

    Utf16String const m_data;
};

}
