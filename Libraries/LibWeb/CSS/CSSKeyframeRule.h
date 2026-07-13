/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-animations/#interface-csskeyframerule
class CSSKeyframeRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSKeyframeRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSKeyframeRule);

public:
    static GC::Ref<CSSKeyframeRule> create(JS::Realm&, CSS::Percentage key, CSSStyleProperties&);

    virtual ~CSSKeyframeRule() = default;

    CSS::Percentage key() const { return m_key; }
    GC::Ref<CSSStyleProperties> style() const { return m_declarations; }

    Utf16String key_text() const
    {
        return m_key.to_utf16_string();
    }

    void set_key_text(Utf16View)
    {
        dbgln("FIXME: CSSKeyframeRule::set_key_text is not implemented");
    }

private:
    CSSKeyframeRule(JS::Realm&, CSS::Percentage, CSSStyleProperties&);

    virtual void visit_edges(Visitor&) override;
    virtual void initialize(JS::Realm&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    CSS::Percentage m_key;
    GC::Ref<CSSStyleProperties> m_declarations;
};

template<>
inline bool CSSRule::fast_is<CSSKeyframeRule>() const { return type() == CSSRule::Type::Keyframe; }

}
