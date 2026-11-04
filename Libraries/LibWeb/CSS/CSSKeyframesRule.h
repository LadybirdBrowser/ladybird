/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-animations/#interface-csskeyframesrule
class CSSKeyframesRule final : public CSSRule {
    WEB_RUST_INTERFACE(CSSKeyframesRule, css_keyframes_rule)
};

template<>
inline bool CSSRule::fast_is<CSSKeyframesRule>() const { return type() == CSSRule::Type::Keyframes; }

}
