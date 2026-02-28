/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

GC::Ref<WebIDL::ObservableArray> create_adopted_style_sheets_list(Node& document_or_shadow_root)
{
    auto adopted_style_sheets = WebIDL::ObservableArray::create(document_or_shadow_root.realm());
    adopted_style_sheets->set_on_set_an_indexed_value_callback([&document_or_shadow_root](JS::Value& value) -> WebIDL::ExceptionOr<void> {
        auto& vm = document_or_shadow_root.vm();
        auto style_sheet = value.as_if<CSS::CSSStyleSheet>();
        if (!style_sheet)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CSSStyleSheet");

        // The set an indexed value algorithm for adoptedStyleSheets, given value and index, is the following:
        // 1. If value’s constructed flag is not set, or its constructor document is not equal to this
        //    DocumentOrShadowRoot's node document, throw a "NotAllowedError" DOMException.
        if (!style_sheet->constructed())
            return WebIDL::NotAllowedError::create(document_or_shadow_root.realm(), "StyleSheet's constructed flag is not set."_utf16);
        if (!style_sheet->constructed() || style_sheet->constructor_document().ptr() != &document_or_shadow_root.document())
            return WebIDL::NotAllowedError::create(document_or_shadow_root.realm(), "Sharing a StyleSheet between documents is not allowed."_utf16);

        style_sheet->add_owning_document_or_shadow_root(document_or_shadow_root);
        auto& style_scope = document_or_shadow_root.is_shadow_root() ? as<DOM::ShadowRoot>(document_or_shadow_root).style_scope() : document_or_shadow_root.document().style_scope();
        style_scope.invalidate_rule_cache();
        document_or_shadow_root.invalidate_style(DOM::StyleInvalidationReason::AdoptedStyleSheetsList);
        return {};
    });
    adopted_style_sheets->set_on_delete_an_indexed_value_callback([&document_or_shadow_root](JS::Value value) -> WebIDL::ExceptionOr<void> {
        auto& style_sheet = value.as<CSS::CSSStyleSheet>();
        auto& style_scope = document_or_shadow_root.is_shadow_root() ? as<DOM::ShadowRoot>(document_or_shadow_root).style_scope() : document_or_shadow_root.document().style_scope();
        style_sheet.remove_owning_document_or_shadow_root(document_or_shadow_root);
        style_scope.invalidate_rule_cache();
        document_or_shadow_root.invalidate_style(DOM::StyleInvalidationReason::AdoptedStyleSheetsList);
        return {};
    });

    return adopted_style_sheets;
}

}
