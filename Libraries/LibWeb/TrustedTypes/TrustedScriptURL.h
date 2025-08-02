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

class TrustedScriptURL final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedScriptURL, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedScriptURL);

public:
    [[nodiscard]] static GC::Ref<TrustedScriptURL> create(JS::Realm&, String const&);

    virtual ~TrustedScriptURL() override { }

    String to_string() const;
    String to_json() const;

    bool data_is_set() const { return m_data.has_value(); }

private:
    explicit TrustedScriptURL(JS::Realm&, String const&);
    virtual void initialize(JS::Realm&) override;

    Optional<String> const m_data;
};

}
