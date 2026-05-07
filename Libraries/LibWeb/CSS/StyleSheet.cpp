/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/Bindings/StyleSheet.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

StyleSheet::StyleSheet(JS::Realm& realm, MediaList& media)
    : PlatformObject(realm)
    , m_media(media)
{
    m_media->set_associated_style_sheet(*this);
}

void StyleSheet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_owner_node);
    visitor.visit(m_parent_style_sheet);
    visitor.visit(m_media);
}

size_t StyleSheet::external_memory_size() const
{
    auto size = Base::external_memory_size();
    size = JS::saturating_add_external_memory_size(size, JS::string_external_memory_size(m_title));
    size = JS::saturating_add_external_memory_size(size, JS::string_external_memory_size(m_type_string));
    return size;
}

Optional<String> StyleSheet::href() const
{
    if (m_location.has_value())
        return m_location->to_string();
    return {};
}

void StyleSheet::set_owner_node(DOM::Element* element)
{
    m_owner_node = element;
}

void StyleSheet::set_parent_css_style_sheet(CSSStyleSheet* parent)
{
    m_parent_style_sheet = parent;
}

// https://drafts.csswg.org/cssom/#dom-stylesheet-title
Optional<String> StyleSheet::title_for_bindings() const
{
    // The title attribute must return the title or null if title is the empty string.
    if (m_title.is_empty())
        return {};

    return m_title;
}

}
