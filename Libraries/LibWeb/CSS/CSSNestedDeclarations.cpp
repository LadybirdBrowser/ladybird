/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNestedDeclarations.h"
#include <AK/NeverDestroyed.h>
#include <LibWeb/Bindings/CSSNestedDeclarations.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNestedDeclarations);

GC::Ref<CSSNestedDeclarations> CSSNestedDeclarations::create(JS::Realm& realm, Parser::Parser& parser, Vector<Parser::Declaration> const& declarations)
{
    return realm.create<CSSNestedDeclarations>(realm, parser.convert_to_style_declaration(declarations));
}

GC::Ref<CSSNestedDeclarations> CSSNestedDeclarations::create(JS::Realm& realm, CSSStyleProperties& declaration)
{
    return realm.create<CSSNestedDeclarations>(realm, declaration);
}

CSSNestedDeclarations::CSSNestedDeclarations(JS::Realm& realm, CSSStyleProperties& declaration)
    : CSSRule(realm, Type::NestedDeclarations)
    , m_declaration(declaration)
{
    m_declaration->set_parent_rule(*this);
}

void CSSNestedDeclarations::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSNestedDeclarations);
    Base::initialize(realm);
}

void CSSNestedDeclarations::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declaration);
    visitor.visit(m_parent_style_rule);
}

static SelectorList absolutize_parent_selectors(CSSNestedDeclarations const& nested_declarations)
{
    static NeverDestroyed<SelectorList> where_scope_selector_list { SelectorList {
        Selector::create({
            Selector::CompoundSelector {
                .combinator = Selector::Combinator::None,
                .simple_selectors = {
                    Selector::SimpleSelector {
                        .type = Selector::SimpleSelector::Type::PseudoClass,
                        .value = Selector::SimpleSelector::PseudoClassSelector {
                            .type = PseudoClass::Where,
                            .argument_selector_list = {
                                Selector::create({
                                    Selector::CompoundSelector {
                                        .combinator = Selector::Combinator::None,
                                        .simple_selectors = {
                                            Selector::SimpleSelector {
                                                .type = Selector::SimpleSelector::Type::PseudoClass,
                                                .value = Selector::SimpleSelector::PseudoClassSelector {
                                                    .type = PseudoClass::Scope,
                                                },
                                            },
                                        },
                                    },
                                }),
                            },
                        },
                    },
                },
            },
        }),
    } };

    for (auto const* parent_rule = nested_declarations.parent_rule(); parent_rule; parent_rule = parent_rule->parent_rule()) {
        if (auto const* parent_style_rule = as_if<CSSStyleRule>(parent_rule))
            return parent_style_rule->absolutized_selectors();
        if (is<CSSScopeRule>(parent_rule)) {
            // https://drafts.csswg.org/css-cascade-6/#scoped-declarations
            // Declarations may be used directly with the body of a @scope rule. Contiguous runs of declarations are
            // wrapped in nested declarations rules, which match the scoping root with zero specificity.
            return *where_scope_selector_list;
        }
    }

    // NB: CSSNestedDeclarations can only exist inside an ancestor rule that provides selectors, so we cannot get here
    //     unless something has gone very wrong.
    VERIFY_NOT_REACHED();
}

SelectorList const& CSSNestedDeclarations::absolutized_selectors() const
{
    if (m_cached_absolutized_selectors.has_value())
        return m_cached_absolutized_selectors.value();

    m_cached_absolutized_selectors = absolutize_parent_selectors(*this);
    return m_cached_absolutized_selectors.value();
}

GC::Ref<CSSStyleProperties> CSSNestedDeclarations::style()
{
    return m_declaration;
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

String CSSNestedDeclarations::serialized() const
{
    // NOTE: There's no proper spec for this yet, only this note:
    // "The CSSNestedDeclarations rule serializes as if its declaration block had been serialized directly."
    // - https://drafts.csswg.org/css-nesting-1/#ref-for-cssnesteddeclarations%E2%91%A1
    // So, we'll do the simple thing and hope it's good.
    return m_declaration->serialized();
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

    dump_style_properties(builder, declaration(), indent_levels + 1);
}

}
