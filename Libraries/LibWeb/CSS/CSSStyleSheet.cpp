/*
 * Copyright (c) 2019-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleSheet);

StyleSheetState* css_style_sheet_from_value(JS::Value value)
{
    auto* sheet = value.is_object() ? Bindings::impl_from<CSSStyleSheet>(&value.as_object()) : nullptr;
    return sheet ? &sheet->state() : nullptr;
}

JS::Value css_style_sheet(JS::Realm& realm, StyleSheetState& style_sheet)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { style_sheet.cssom_sheet() });
}

GC::Ref<JS::SyntheticModule> create_css_style_sheet_default_export_module(JS::Realm& realm, StyleSheetState& style_sheet, StringView filename)
{
    return JS::SyntheticModule::create_default_export_synthetic_module(realm, css_style_sheet(realm, style_sheet), filename);
}

WebIDL::ExceptionOr<GC::Ref<CSSStyleSheet>> CSSStyleSheet::create_for_constructor(JS::Object& relevant_global_object, CSSStyleSheetOptions const& options)
{
    auto associated_document = HTML::relevant_window(relevant_global_object).document();
    auto state = TRY(StyleSheetState::create_constructed(*associated_document, options));
    return GC::Ref { state->cssom_sheet() };
}

GC::Ref<CSSStyleSheet> CSSStyleSheet::create(StyleSheetState& state)
{
    return GC::Heap::the().allocate<CSSStyleSheet>(state);
}

CSSStyleSheet::CSSStyleSheet(StyleSheetState& state)
    : StyleSheet(state)
{
    update_owner_chain();
}

CSSStyleSheet::~CSSStyleSheet() = default;

void CSSStyleSheet::update_owner_chain()
{
    auto* parent = state().parent_style_sheet();
    m_parent_sheet = parent ? &parent->cssom_sheet() : nullptr;
    m_owner_import = state().owner_import();
}

void CSSStyleSheet::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_sheet);
    visitor.visit(m_owner_import);
}

}
