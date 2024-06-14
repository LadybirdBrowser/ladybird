/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NonnullRefPtr.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-animations/#interface-csskeyframesrule
class CSSKeyframesRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSKeyframesRule, CSSRule);
    JS_DECLARE_ALLOCATOR(CSSKeyframesRule);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSKeyframesRule> create(JS::Realm&, FlyString name, JS::NonnullGCPtr<CSSRuleList>);

    virtual ~CSSKeyframesRule() = default;

    virtual Type type() const override { return Type::Keyframes; }

    auto const& css_rules() const { return m_rules; }
    FlyString const& name() const { return m_name; }
    [[nodiscard]] WebIDL::UnsignedLong length() const;

    void set_name(String const& name) { m_name = name; }

private:
    CSSKeyframesRule(JS::Realm&, FlyString name, JS::NonnullGCPtr<CSSRuleList> keyframes);
    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;

    FlyString m_name;
    JS::NonnullGCPtr<CSSRuleList> m_rules;
};

template<>
inline bool CSSRule::fast_is<CSSKeyframesRule>() const { return type() == CSSRule::Type::Keyframes; }

}
