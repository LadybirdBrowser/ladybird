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
    WEB_WRAPPABLE(CSSImageValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSImageValue);

public:
    [[nodiscard]] static GC::Ref<CSSImageValue> create(NonnullRefPtr<StyleValue const> source_value);

    virtual ~CSSImageValue() override = default;

    virtual WebIDL::ExceptionOr<String> to_string() const override;
    virtual WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const override;

private:
    explicit CSSImageValue(NonnullRefPtr<StyleValue const> source_value);
};

}
