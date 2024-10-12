/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

JS::NonnullGCPtr<WebIDL::ObservableArray> create_adopted_style_sheets_list(Document& document)
{
    auto adopted_style_sheets = WebIDL::ObservableArray::create(document.realm());
    adopted_style_sheets->set_on_set_an_indexed_value_callback([&document](JS::Value& value) -> WebIDL::ExceptionOr<void> {
        auto& vm = document.vm();
        if (!value.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CSSStyleSheet");
        auto& object = value.as_object();
        if (!is<CSS::CSSStyleSheet>(object))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CSSStyleSheet");
        auto& style_sheet = static_cast<CSS::CSSStyleSheet&>(object);

        // The set an indexed value algorithm for adoptedStyleSheets, given value and index, is the following:
        // 1. If value’s constructed flag is not set, or its constructor document is not equal to this
        //    DocumentOrShadowRoot's node document, throw a "NotAllowedError" DOMException.
        if (!style_sheet.constructed())
            return WebIDL::NotAllowedError::create(document.realm(), "StyleSheet's constructed flag is not set."_string);
        if (!style_sheet.constructed() || style_sheet.constructor_document().ptr() != &document)
            return WebIDL::NotAllowedError::create(document.realm(), "Sharing a StyleSheet between documents is not allowed."_string);

        document.style_computer().load_fonts_from_sheet(style_sheet);
        document.style_computer().invalidate_rule_cache();
        document.invalidate_style(DOM::StyleInvalidationReason::AdoptedStyleSheetsList);
        return {};
    });
    adopted_style_sheets->set_on_delete_an_indexed_value_callback([&document]() -> WebIDL::ExceptionOr<void> {
        document.style_computer().invalidate_rule_cache();
        document.invalidate_style(DOM::StyleInvalidationReason::AdoptedStyleSheetsList);
        return {};
    });

    return adopted_style_sheets;
}

}
