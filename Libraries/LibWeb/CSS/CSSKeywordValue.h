/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/CSSKeywordValue.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csskeywordvalue
class WEB_API CSSKeywordValue final : public CSSStyleValue {
    WEB_RUST_INTERFACE(CSSKeywordValue, css_keyword_value)

public:
    Utf16String value() const;
};

}
