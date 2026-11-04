/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustFontFeatureValues.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CSSFontFeatureValuesRule final : public CSSRule {
    WEB_RUST_INTERFACE(CSSFontFeatureValuesRule, css_font_feature_values_rule)

public:
    Parser::ValueParserFFI::FontFeatureValuesRule const& values() const { return *native_rule().payload().font_feature_values; }

    void clear_caches();

private:
    Vector<Utf16FlyString> font_families() const;
};

}
