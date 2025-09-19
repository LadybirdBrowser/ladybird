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
    [[nodiscard]] static GC::Ref<CSSImageValue> create(JS::Realm&, NonnullRefPtr<StyleValue const> source_value);

    virtual ~CSSImageValue() override = default;

    virtual WebIDL::ExceptionOr<String> to_string() const override;

private:
    explicit CSSImageValue(JS::Realm&, NonnullRefPtr<StyleValue const> source_value);

    virtual void initialize(JS::Realm&) override;
};

}
