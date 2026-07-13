/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerStatementRule.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/CSSLayerStatementRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSLayerStatementRule);

GC::Ref<CSSLayerStatementRule> CSSLayerStatementRule::create(JS::Realm& realm, Vector<Utf16FlyString> name_list)
{
    return realm.create<CSSLayerStatementRule>(realm, move(name_list));
}

CSSLayerStatementRule::CSSLayerStatementRule(JS::Realm& realm, Vector<Utf16FlyString> name_list)
    : CSSRule(realm, Type::LayerStatement)
    , m_name_list(move(name_list))
{
}

void CSSLayerStatementRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSLayerStatementRule);
    Base::initialize(realm);
}

Utf16String CSSLayerStatementRule::serialized() const
{
    // AD-HOC: No spec yet.
    Utf16StringBuilder builder;
    builder.append_ascii("@layer "sv);
    for (size_t i = 0; i < m_name_list.size(); ++i) {
        if (i > 0)
            builder.append_ascii(", "sv);
        builder.append(m_name_list[i]);
    }
    builder.append_ascii(';');
    return builder.to_string();
}

Vector<Utf16FlyString> CSSLayerStatementRule::internal_qualified_name_list(Badge<StyleScope>) const
{
    Vector<Utf16FlyString> qualified_layer_names;

    auto const& qualified_parent_layer_name = parent_layer_internal_qualified_name();
    if (qualified_parent_layer_name.is_empty())
        return m_name_list;

    for (auto const& name : m_name_list) {
        Utf16StringBuilder builder;
        builder.append(qualified_parent_layer_name);
        builder.append_ascii('.');
        builder.append(name);
        auto qualified_name = builder.to_string();
        qualified_layer_names.append(Utf16FlyString::from_utf16(qualified_name.utf16_view()));
    }

    return qualified_layer_names;
}

void CSSLayerStatementRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.append("Names: "sv);
    builder.join(", "sv, name_list());
}

}
