/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
class CSSStyleValue : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSStyleValue, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSStyleValue);

public:
    [[nodiscard]] static GC::Ref<CSSStyleValue> create(JS::Realm&, String associated_property, String constructed_from_string);

    virtual ~CSSStyleValue() override = default;

    virtual void initialize(JS::Realm&) override;

    virtual String to_string() const;

protected:
    explicit CSSStyleValue(JS::Realm&);

private:
    explicit CSSStyleValue(JS::Realm&, String associated_property, String constructed_from_string);

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-associatedproperty-slot
    Optional<String> m_associated_property;

    Optional<String> m_constructed_from_string;
};

}
