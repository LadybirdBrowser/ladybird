/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleSheetList);

GC::Ref<StyleSheetList> StyleSheetList::create(StyleScope& scope)
{
    return GC::Heap::the().allocate<StyleSheetList>(scope);
}

StyleSheetList::StyleSheetList(StyleScope& scope)
    : m_scope(scope)
{
}

CSSStyleSheet* StyleSheetList::item(size_t index) const
{
    auto const& sheets = m_scope.style_sheets();
    if (index >= sheets.size())
        return nullptr;
    return &sheets[index]->cssom_sheet();
}

size_t StyleSheetList::length() const
{
    return m_scope.style_sheets().size();
}

void StyleSheetList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_scope.node());
}

}
