/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerStatementRule.h"
#include <LibWeb/Bindings/CSSLayerStatementRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSLayerStatementRule);

JS::NonnullGCPtr<CSSLayerStatementRule> CSSLayerStatementRule::create(JS::Realm& realm, Vector<FlyString> name_list)
{
    return realm.heap().allocate<CSSLayerStatementRule>(realm, realm, move(name_list));
}

CSSLayerStatementRule::CSSLayerStatementRule(JS::Realm& realm, Vector<FlyString> name_list)
    : CSSRule(realm)
    , m_name_list(move(name_list))
{
}

void CSSLayerStatementRule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSLayerStatementRule);
}

String CSSLayerStatementRule::serialized() const
{
    // AD-HOC: No spec yet.
    StringBuilder builder;
    builder.append("@layer "sv);
    builder.join(", "sv, m_name_list);
    builder.append(';');
    return builder.to_string_without_validation();
}

Vector<FlyString> CSSLayerStatementRule::internal_qualified_name_list(Badge<StyleComputer>) const
{
    Vector<FlyString> qualified_layer_names;

    auto const& qualified_parent_layer_name = parent_layer_internal_qualified_name();
    if (qualified_parent_layer_name.is_empty())
        return m_name_list;

    for (auto const& name : m_name_list)
        qualified_layer_names.append(MUST(String::formatted("{}.{}", qualified_parent_layer_name, name)));

    return qualified_layer_names;
}

}
