/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSConditionRule.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-conditional-3/#the-cssmediarule-interface
class CSSMediaRule final : public CSSConditionRule {
    WEB_WRAPPABLE(CSSMediaRule, CSSConditionRule);
    GC_DECLARE_ALLOCATOR(CSSMediaRule);

public:
    [[nodiscard]] static GC::Ref<CSSMediaRule> create(RustRule, CSSRuleList&);

    virtual ~CSSMediaRule() = default;

    virtual Utf16String serialized_condition_text() const override;
    bool matches() const { return m_media_list.matches(); }

    MediaList* media() const;
    RustMediaList const& native_media_list() const { return m_media_list; }

private:
    CSSMediaRule(RustRule, CSSRuleList&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    RustMediaList m_media_list;
    mutable GC::Ptr<MediaList> m_media;
};

template<>
inline bool CSSRule::fast_is<CSSMediaRule>() const { return type() == CSSRule::Type::Media; }

}
