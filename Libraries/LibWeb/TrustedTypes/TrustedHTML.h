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
    [[nodiscard]] static GC::Ref<TrustedHTML> create(JS::Realm&, String const&);

    virtual ~TrustedHTML() override { }

    String to_string() const;
    String to_json() const;

    bool data_is_set() const { return m_data.has_value(); }

private:
    explicit TrustedHTML(JS::Realm&, String const&);
    virtual void initialize(JS::Realm&) override;

    Optional<String> const m_data;
};

}
