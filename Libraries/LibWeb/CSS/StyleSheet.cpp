/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleSheet.h>

namespace Web::CSS {

StyleSheet::StyleSheet(StyleSheetState& state)
    : m_state(state)
{
}

void StyleSheet::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_state);
}

CSSStyleSheet* StyleSheet::parent_style_sheet() const
{
    auto* parent = m_state->parent_style_sheet();
    return parent ? &parent->cssom_sheet() : nullptr;
}

size_t StyleSheet::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_state->external_memory_size());
}

}
