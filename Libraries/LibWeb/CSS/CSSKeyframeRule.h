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
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-animations/#interface-csskeyframerule
class CSSKeyframeRule final : public CSSRule {
    WEB_WRAPPABLE(CSSKeyframeRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSKeyframeRule);

public:
    static GC::Ref<CSSKeyframeRule> create(RustRule);

    virtual ~CSSKeyframeRule() = default;

    GC::Ref<CSSStyleProperties> style() const;

    Utf16String key_text() const
    {
        Utf16StringBuilder builder;
        auto keys = Parser::ValueParserFFI::rust_keyframe_keys(&m_frame);
        for (auto key : ReadonlySpan<double> { keys.values, keys.count }) {
            if (!builder.is_empty())
                builder.append(", "sv);
            builder.appendff("{}%"sv, key);
        }

        return builder.to_string();
    }

    void set_key_text(Utf16View)
    {
        dbgln("FIXME: CSSKeyframeRule::set_key_text is not implemented");
    }

private:
    CSSKeyframeRule(RustRule);

    virtual size_t external_memory_size() const override;
    virtual void visit_edges(Visitor&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Parser::ValueParserFFI::FfiKeyframe const& m_frame;
    RustDeclarationBlock m_declarations;
    mutable GC::Ptr<CSSStyleProperties> m_style;
};

template<>
inline bool CSSRule::fast_is<CSSKeyframeRule>() const { return type() == CSSRule::Type::Keyframe; }

}
