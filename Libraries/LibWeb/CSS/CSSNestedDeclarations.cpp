/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNestedDeclarations.h"
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNestedDeclarations);

GC::Ref<CSSNestedDeclarations> CSSNestedDeclarations::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSNestedDeclarations>(move(rule));
}

CSSNestedDeclarations::CSSNestedDeclarations(RustRule rule)
    : CSSRule(move(rule))
    , m_declarations(Parser::ValueParserFFI::rust_declaration_block_retain(native_rule().payload().declarations))
{
}

size_t CSSNestedDeclarations::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_declarations.external_memory_size());
}

GC::Ref<CSSStyleProperties> CSSNestedDeclarations::ensure_style_properties() const
{
    if (!m_declaration) {
        m_declaration = CSSStyleProperties::create(m_declarations.retain());
        m_declaration->set_parent_rule(const_cast<CSSNestedDeclarations&>(*this));
    }
    return *m_declaration;
}

void CSSNestedDeclarations::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declaration);
    visitor.visit(m_parent_style_rule);
}

SelectorList const& CSSNestedDeclarations::absolutized_selectors() const
{
    if (m_cached_absolutized_selectors.has_value())
        return m_cached_absolutized_selectors.value();

    m_cached_absolutized_selectors = matching_selectors_for_rule(native_rule());
    return m_cached_absolutized_selectors.value();
}

GC::Ref<CSSStyleProperties> CSSNestedDeclarations::style() const
{
    return ensure_style_properties();
}

CSSStyleRule const& CSSNestedDeclarations::parent_style_rule() const
{
    if (m_parent_style_rule)
        return *m_parent_style_rule;

    for (auto* parent = parent_rule(); parent; parent = parent->parent_rule()) {
        if (is<CSSStyleRule>(parent)) {
            m_parent_style_rule = static_cast<CSSStyleRule const*>(parent);
            return *m_parent_style_rule;
        }
    }

    dbgln("CSSNestedDeclarations has no parent style rule!");
    VERIFY_NOT_REACHED();
}

Utf16String CSSNestedDeclarations::serialized() const
{
    // NOTE: There's no proper spec for this yet, only this note:
    // "The CSSNestedDeclarations rule serializes as if its declaration block had been serialized directly."
    // - https://drafts.csswg.org/css-nesting-1/#ref-for-cssnesteddeclarations%E2%91%A1
    // So, we'll do the simple thing and hope it's good.
    return ensure_style_properties()->serialized();
}

void CSSNestedDeclarations::clear_caches()
{
    Base::clear_caches();
    m_parent_style_rule = nullptr;
    m_cached_absolutized_selectors.clear();
}

void CSSNestedDeclarations::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_style_properties(builder, *ensure_style_properties(), indent_levels + 1);
}

}
