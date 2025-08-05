/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedScriptPrototype.h>

namespace Web::TrustedTypes {

using TrustedScriptOrString = Variant<GC::Root<TrustedScript>, Utf16String>;

class TrustedScript final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedScript, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedScript);

public:
    virtual ~TrustedScript() override = default;

    Utf16String const& to_string() const;
    Utf16String const& to_json() const;

private:
    explicit TrustedScript(JS::Realm&, Utf16String);
    virtual void initialize(JS::Realm&) override;

    Utf16String const m_data;
};

}
