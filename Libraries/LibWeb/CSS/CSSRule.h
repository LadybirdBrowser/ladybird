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
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

class WEB_API CSSRule : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CSSRule, Bindings::GCAllocatedWrappable);

public:
    virtual ~CSSRule() = default;

    // https://drafts.csswg.org/cssom/#dom-cssrule-type
    enum class Type : WebIDL::UnsignedShort {
        Style = 1,
        Import = 3,
        Media = 4,
        FontFace = 5,
        Page = 6,
        Keyframes = 7,
        Keyframe = 8,
        Margin = 9,
        Namespace = 10,
        CounterStyle = 11,
        Supports = 12,
        FontFeatureValues = 14,
        // AD-HOC: These are not included in the spec, but we need them internally. So, their numbers are arbitrary.
        LayerBlock = 100,
        LayerStatement = 101,
        NestedDeclarations = 102,
        Property = 103,
        Function = 104,
        FunctionDeclarations = 105,
        Container = 106,
        Scope = 107,
    };

    Type type() const { return m_type; }
    WebIDL::UnsignedShort type_for_bindings() const;

    Utf16String css_text() const;
    void set_css_text(Utf16View);

    CSSRule* parent_rule() { return m_parent_rule.ptr(); }
    CSSRule const* parent_rule() const { return m_parent_rule.ptr(); }
    void set_parent_rule(CSSRule*);
    static constexpr size_t parent_rule_offset() { return offsetof(CSSRule, m_parent_rule); }

    CSSStyleSheet* parent_style_sheet() { return m_parent_style_sheet.ptr(); }
    CSSStyleSheet const* parent_style_sheet() const { return m_parent_style_sheet.ptr(); }
    MUST_UPCALL virtual void set_parent_style_sheet(CSSStyleSheet*);
    static constexpr size_t parent_style_sheet_offset() { return offsetof(CSSRule, m_parent_style_sheet); }

    Optional<SourcePosition> const& source_location() const { return m_source_position; }
    void set_source_position(Optional<SourcePosition> source_location) { m_source_position = move(source_location); }

    template<typename T>
    bool fast_is() const = delete;

    // https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
    virtual Utf16String serialized() const = 0;

    MUST_UPCALL virtual void dump(StringBuilder&, int indent_levels = 0) const;

    MUST_UPCALL virtual void clear_caches();

    // The StyleEngine rule this CSSOM rule compiled into, or 0 if it has not been compiled. The
    // engine's rule is a separate identity that keeps its position and its cascade order across
    // edits to what this rule says.
    [[nodiscard]] StyleEngineRuleID style_engine_rule_id() const { return m_style_engine_rule_id; }
    void set_style_engine_rule_id(StyleEngineRuleID rule_id) { m_style_engine_rule_id = rule_id; }

protected:
    CSSRule(Type);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    [[nodiscard]] Utf16FlyString const& parent_layer_internal_qualified_name() const
    {
        return m_cached_layer_name.ensure([&] { return parent_layer_internal_qualified_name_slow_case(); });
    }

    [[nodiscard]] Utf16FlyString parent_layer_internal_qualified_name_slow_case() const;

    Type m_type;
    GC::Ptr<CSSRule> m_parent_rule;
    GC::Ptr<CSSStyleSheet> m_parent_style_sheet;

    Optional<SourcePosition> m_source_position;
    mutable Optional<Utf16FlyString> m_cached_layer_name;
    StyleEngineRuleID m_style_engine_rule_id;
};

}
