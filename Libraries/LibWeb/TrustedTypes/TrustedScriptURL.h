/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedScriptURLPrototype.h>

namespace Web::TrustedTypes {

using TrustedScriptURLOrString = Variant<GC::Root<TrustedScriptURL>, Utf16String>;

class TrustedScriptURL final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedScriptURL, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedScriptURL);

public:
    virtual ~TrustedScriptURL() override = default;

    Utf16String const& to_string() const;
    Utf16String const& to_json() const;

private:
    explicit TrustedScriptURL(JS::Realm&, Utf16String);
    virtual void initialize(JS::Realm&) override;

    Utf16String const m_data;
};

}
