/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/CSSRuleList.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustRule.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom/#the-cssrulelist-interface
class WEB_API CSSRuleList final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CSSRuleList, css_rule_list)

public:
    [[nodiscard]] static GC::Ref<CSSRuleList> create(RustRuleList const&, GC::Ptr<DOM::Document>);

    CSSRule* item(size_t index) const;
    size_t length() const;
    RustRuleList native_rules() const;
    CSSRule* existing_wrapper(u64 identity) const;
    void set_parent_style_sheet(StyleSheetState*);
    CSSRule* rule_for_identity(u64) const;

    class Iterator {
    public:
        Iterator(CSSRuleList const& list, size_t index)
            : m_list(list)
            , m_index(index)
        {
        }
        GC::Ref<CSSRule> operator*() const { return *m_list->item(m_index); }
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
    void remove_a_css_rule_without_validation(Badge<StyleSheetState>, u32 index);
    enum class Nested {
        No,
        Yes,
    };
    static WebIDL::ExceptionOr<unsigned> insert_a_css_rule(RustRuleList&, RustRule const&, u32 index, Nested);

    void set_rules(Badge<StyleSheetState>, RustRuleList const&, GC::Ptr<DOM::Document>);
};

}
