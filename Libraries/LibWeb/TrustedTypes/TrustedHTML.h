/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedHTMLPrototype.h>

namespace Web::TrustedTypes {

class TrustedHTML final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedHTML, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedHTML);

public:
    virtual ~TrustedHTML() override = default;

    Utf16String const& to_string() const;
    Utf16String const& to_json() const;

private:
    explicit TrustedHTML(JS::Realm&, Utf16String);
    virtual void initialize(JS::Realm&) override;

    Utf16String const m_data;
};

}
