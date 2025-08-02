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

class TrustedScript final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedScript, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedScript);

public:
    [[nodiscard]] static GC::Ref<TrustedScript> create(JS::Realm&, String const&);

    virtual ~TrustedScript() override { }

    String to_string() const;
    String to_json() const;

    bool data_is_set() const { return m_data.has_value(); }

private:
    explicit TrustedScript(JS::Realm&, String const&);
    virtual void initialize(JS::Realm&) override;

    Optional<String> const m_data;
};

}
