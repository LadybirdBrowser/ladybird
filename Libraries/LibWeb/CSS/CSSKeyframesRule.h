/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-animations/#interface-csskeyframesrule
class CSSKeyframesRule final : public CSSRule {
    WEB_WRAPPABLE(CSSKeyframesRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSKeyframesRule);

public:
    static constexpr size_t rules_offset() { return offsetof(CSSKeyframesRule, m_rules); }
    [[nodiscard]] static GC::Ref<CSSKeyframesRule> create(Utf16FlyString name, GC::Ref<CSSRuleList>);

    virtual ~CSSKeyframesRule() = default;

    auto const& css_rules() const { return m_rules; }
    Utf16FlyString const& name() const { return m_name; }
    [[nodiscard]] WebIDL::UnsignedLong length() const;
    GC::Ptr<CSSKeyframeRule> item(size_t index) const;

    void set_name(Utf16String const& name);

    void append_rule(Utf16String const& rule);
    void delete_rule(Utf16String const& select);
    GC::Ptr<CSSKeyframeRule> find_rule(Utf16String const& select);

    // A keyframes rule holds rules without being a grouping rule, so its keyframes have to be handed
    // the sheet themselves. A keyframe edited through the CSSOM reports against the sheet it belongs
    // to, and one that never learned its sheet reports against nothing at all.
    virtual void set_parent_style_sheet(CSSStyleSheet*) override;

private:
    CSSKeyframesRule(Utf16FlyString name, GC::Ref<CSSRuleList> keyframes);
    virtual void visit_edges(Visitor&) override;

    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_name;
    GC::Ref<CSSRuleList> m_rules;
};

template<>
inline bool CSSRule::fast_is<CSSKeyframesRule>() const { return type() == CSSRule::Type::Keyframes; }

}
