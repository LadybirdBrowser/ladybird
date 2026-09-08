/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom/#the-cssrulelist-interface
class WEB_API CSSRuleList : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CSSRuleList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CSSRuleList);

public:
    [[nodiscard]] static GC::Ref<CSSRuleList> create(RustRuleList, GC::Ptr<DOM::Document>);

    virtual ~CSSRuleList() override;

    CSSRule const* item(size_t index) const
    {
        if (index >= length())
            return nullptr;
        return wrapper_at(index).ptr();
    }

    CSSRule* item(size_t index)
    {
        if (index >= length())
            return nullptr;
        return wrapper_at(index).ptr();
    }

    size_t length() const { return m_rules.size(); }
    RustRuleList const& native_rules() const { return m_rules; }
    CSSRule* existing_wrapper(u64 identity) const;
    void for_each_existing_rule(Function<void(CSSRule&)> const&) const;
    void set_parent_style_sheet(StyleSheetState*);
    CSSRule const* rule_for_identity(u64) const;
    CSSRule* rule_for_identity(u64 identity) { return const_cast<CSSRule*>(std::as_const(*this).rule_for_identity(identity)); }

    class Iterator {
    public:
        Iterator(CSSRuleList const& list, size_t index)
            : m_list(list)
            , m_index(index)
        {
        }
        GC::Ref<CSSRule> const& operator*() const { return m_list->wrapper_at(m_index); }
        Iterator& operator++()
        {
            ++m_index;
            return *this;
        }
        bool operator==(Iterator const& other) const { return m_list == other.m_list && m_index == other.m_index; }

    private:
        GC::Ref<CSSRuleList const> m_list;
        size_t m_index;
    };

    Iterator begin() const { return { *this, 0 }; }
    Iterator end() const { return { *this, length() }; }

    static WebIDL::ExceptionOr<void> validate_rule_removal(RustRuleList const&, u32 index);
    WebIDL::ExceptionOr<RustRule> remove_a_css_rule(u32 index);
    void remove_a_css_rule_without_validation(Badge<StyleSheetState>, u32 index);
    enum class Nested {
        No,
        Yes,
    };
    static WebIDL::ExceptionOr<unsigned> insert_a_css_rule(RustRuleList&, RustRule const&, u32 index, Nested);
    WebIDL::ExceptionOr<unsigned> insert_a_css_rule(Utf16View, u32 index, Nested, RustNamespaceContext const& declared_namespaces);

    void set_owner_rule(GC::Ref<CSSRule>);
    void set_rules(Badge<StyleSheetState>, RustRuleList, GC::Ptr<DOM::Document>);

    Function<void()> on_change;

private:
    CSSRuleList(RustRuleList, GC::Ptr<DOM::Document>);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    void remove_a_css_rule_without_validation(u32 index);
    Vector<Parser::RuleContext> rule_context() const;
    GC::Ref<CSSRule> const& wrapper_at(size_t index) const;

    RustRuleList m_rules;
    mutable HashMap<u64, GC::Ref<CSSRule>> m_wrappers;
    GC::Ptr<CSSRule> m_owner_rule;
    RefPtr<StyleSheetState> m_parent_style_sheet;
    GC::Ptr<CSSStyleSheet> m_parent_cssom_sheet;
    GC::Ptr<DOM::Document> m_document;
};

}
