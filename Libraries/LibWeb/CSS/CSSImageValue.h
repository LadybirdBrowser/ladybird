/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssimagevalue
class CSSImageValue final : public CSSStyleValue {
    WEB_PLATFORM_OBJECT(CSSImageValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSImageValue);

public:
    [[nodiscard]] static GC::Ref<CSSImageValue> create(JS::Realm&, String constructed_from_string);

    virtual ~CSSImageValue() override = default;

    virtual String to_string() const override;

private:
    explicit CSSImageValue(JS::Realm&, String constructed_from_string);

    virtual void initialize(JS::Realm&) override;

    String m_constructed_from_string;
};

}
