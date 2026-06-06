/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/TrustedScriptURL.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::TrustedTypes {

using TrustedScriptURLOrString = Variant<GC::Ref<TrustedScriptURL>, Utf16String>;

class TrustedScriptURL final : public Bindings::Wrappable {
    WEB_WRAPPABLE(TrustedScriptURL, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TrustedScriptURL);

public:
    virtual ~TrustedScriptURL() override = default;

    Utf16String const& to_string() const;
    Utf16String const& to_json() const;

private:
    explicit TrustedScriptURL(Utf16String);

    Utf16String const m_data;
};

}
