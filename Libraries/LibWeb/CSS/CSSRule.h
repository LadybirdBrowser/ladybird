/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Parser/SourcePosition.h>
#include <LibWeb/CSS/RustRule.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

class WEB_API CSSRule : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CSSRule, Bindings::GCAllocatedWrappable);

public:
    static GC::Ref<CSSRule> create(RustRule, GC::Ptr<DOM::Document>);
    virtual ~CSSRule() override;

    using Type = RustRule::Type;

    Type type() const { return m_type; }
    RustRule const& native_rule() const { return m_native_rule; }
    WebIDL::UnsignedShort type_for_bindings() const;

    Utf16String css_text() const;
    void set_css_text(Utf16View);

    CSSRule* parent_rule() { return m_parent_rule.ptr(); }
    CSSRule const* parent_rule() const { return m_parent_rule.ptr(); }
    void set_parent_rule(CSSRule*);
    static constexpr size_t parent_rule_offset() { return offsetof(CSSRule, m_parent_rule); }

    StyleSheetState* parent_style_sheet() { return m_parent_style_sheet.ptr(); }
    StyleSheetState const* parent_style_sheet() const { return m_parent_style_sheet.ptr(); }
    MUST_UPCALL virtual void set_parent_style_sheet(StyleSheetState*);
    CSSStyleSheet* parent_style_sheet_for_bindings() const;

    Optional<SourcePosition> const& source_location() const { return m_source_position; }

    template<typename T>
    bool fast_is() const = delete;

    // https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
    virtual Utf16String serialized() const = 0;

    MUST_UPCALL virtual void dump(StringBuilder&, int indent_levels = 0) const;

    MUST_UPCALL virtual void clear_caches();

protected:
    explicit CSSRule(RustRule);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    [[nodiscard]] Utf16FlyString const& parent_layer_internal_qualified_name() const
    {
        return m_cached_layer_name.ensure([&] { return parent_layer_internal_qualified_name_slow_case(); });
    }

    [[nodiscard]] Utf16FlyString parent_layer_internal_qualified_name_slow_case() const;

    Type m_type;
    RustRule m_native_rule;
    GC::Ptr<CSSRule> m_parent_rule;
    RefPtr<StyleSheetState> m_parent_style_sheet;
    GC::Ptr<CSSStyleSheet> m_parent_cssom_sheet;

    Optional<SourcePosition> m_source_position;
    mutable Optional<Utf16FlyString> m_cached_layer_name;
};

}
