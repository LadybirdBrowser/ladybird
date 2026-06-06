/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSStyleSheet.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Invalidation/AdoptedStyleSheetInvalidator.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

static CSS::CSSStyleSheet* css_style_sheet_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;

    if (auto* style_sheet = Bindings::impl_from<CSS::CSSStyleSheet>(&value.as_object()))
        return style_sheet;

    return nullptr;
}

GC::Ref<WebIDL::ObservableArray> create_adopted_style_sheets_list(Node& document_or_shadow_root)
{
    auto adopted_style_sheets = WebIDL::ObservableArray::create(document_or_shadow_root.realm());
    adopted_style_sheets->set_on_set_an_indexed_value_callback([&document_or_shadow_root](JS::Value& value) -> WebIDL::ExceptionOr<void> {
        auto& vm = document_or_shadow_root.vm();
        auto style_sheet = css_style_sheet_from_value(value);
        if (!style_sheet)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CSSStyleSheet");

        // The set an indexed value algorithm for adoptedStyleSheets, given value and index, is the following:
        // 1. If value’s constructed flag is not set, or its constructor document is not equal to this
        //    DocumentOrShadowRoot's node document, throw a "NotAllowedError" DOMException.
        if (!style_sheet->constructed())
            return WebIDL::NotAllowedError::create(document_or_shadow_root.realm(), "StyleSheet's constructed flag is not set."_utf16);
        if (!style_sheet->constructed() || style_sheet->constructor_document().ptr() != &document_or_shadow_root.document())
            return WebIDL::NotAllowedError::create(document_or_shadow_root.realm(), "Sharing a StyleSheet between documents is not allowed."_utf16);

        CSS::Invalidation::invalidate_style_after_adopting_style_sheet(document_or_shadow_root, *style_sheet);
        return {};
    });
    adopted_style_sheets->set_on_delete_an_indexed_value_callback([&document_or_shadow_root](JS::Value value) -> WebIDL::ExceptionOr<void> {
        if (auto style_sheet = css_style_sheet_from_value(value))
            CSS::Invalidation::invalidate_style_after_removing_adopted_style_sheet(document_or_shadow_root, *style_sheet);
        return {};
    });

    return adopted_style_sheets;
}

void for_each_adopted_style_sheet(WebIDL::ObservableArray& adopted_style_sheets, Function<void(CSS::CSSStyleSheet&)> const& callback)
{
    for (u32 i = 0; i < adopted_style_sheets.indexed_array_like_size(); ++i) {
        auto value_and_attributes = adopted_style_sheets.indexed_get(i);
        if (!value_and_attributes.has_value())
            continue;

        if (auto style_sheet = css_style_sheet_from_value(value_and_attributes->value))
            callback(*style_sheet);
    }
}

}
