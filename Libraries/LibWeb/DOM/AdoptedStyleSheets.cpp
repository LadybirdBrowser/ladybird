/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/AdoptedStyleSheetInvalidator.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::DOM {

GC::Ref<WebIDL::ObservableArray> AdoptedStyleSheetsAccess::adopted_style_sheets(Document& document)
{
    return document.adopted_style_sheets();
}

GC::Ref<WebIDL::ObservableArray> AdoptedStyleSheetsAccess::adopted_style_sheets(ShadowRoot& shadow_root)
{
    return shadow_root.adopted_style_sheets();
}

// NB: The engine holds each sheet once. A sheet that the array holds more than once is placed at its last occurrence,
//     where its rules win the cascade.
static Optional<u32> last_index_of(WebIDL::ObservableArray& adopted_style_sheets, CSS::StyleSheetState& style_sheet, u32 skipped_index)
{
    for (auto index = adopted_style_sheets.length(); index > 0;) {
        --index;
        if (index == skipped_index)
            continue;
        if (auto value = adopted_style_sheets.element_value(index); value.has_value() && CSS::css_style_sheet_from_value(*value) == &style_sheet)
            return index;
    }
    return {};
}

// The first sheet after index that the engine places there, which is one that does not occur again later.
static CSS::StyleSheetState* first_sheet_after(WebIDL::ObservableArray& adopted_style_sheets, u32 index, u32 skipped_index)
{
    for (auto next_index = index + 1; next_index < adopted_style_sheets.length(); ++next_index) {
        if (next_index == skipped_index)
            continue;
        if (auto value = adopted_style_sheets.element_value(next_index); value.has_value()) {
            auto* style_sheet = CSS::css_style_sheet_from_value(*value);
            if (style_sheet && last_index_of(adopted_style_sheets, *style_sheet, skipped_index) == next_index)
                return style_sheet;
        }
    }
    return nullptr;
}

GC::Ref<WebIDL::ObservableArray> create_adopted_style_sheets_list(Node& document_or_shadow_root)
{
    auto& realm = document_or_shadow_root.document().relevant_settings_object().realm();
    auto adopted_style_sheets = WebIDL::ObservableArray::create(realm);
    adopted_style_sheets->set_convert_to_idl_value_callback(GC::create_function(realm.heap(), GC::captures(document_or_shadow_root), [](Node& document_or_shadow_root, JS::Value value) -> WebIDL::ExceptionOr<void> {
        auto style_sheet = CSS::css_style_sheet_from_value(value);
        if (!style_sheet)
            return document_or_shadow_root.vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CSSStyleSheet");

        // NB: These are the steps of the set an indexed value algorithm, which runs after the delete algorithm for
        //     the value it replaces. That delete algorithm has no effect that script can observe, so checking first
        //     is not observable either, and keeps a rejected replacement from detaching the sheet that stays.
        // The set an indexed value algorithm for adoptedStyleSheets, given value and index, is the following:
        // 1. If value’s constructed flag is not set, or its constructor document is not equal to this
        //    DocumentOrShadowRoot's node document, throw a "NotAllowedError" DOMException.
        if (!style_sheet->constructed())
            return WebIDL::NotAllowedError::create("StyleSheet's constructed flag is not set."_utf16);
        if (!style_sheet->constructed() || style_sheet->constructor_document().ptr() != &document_or_shadow_root.document())
            return WebIDL::NotAllowedError::create("Sharing a StyleSheet between documents is not allowed."_utf16);
        return {};
    }));
    adopted_style_sheets->set_on_set_an_indexed_value_callback(GC::create_function(realm.heap(), GC::captures(document_or_shadow_root, adopted_style_sheets), [](Node& document_or_shadow_root, GC::Ref<WebIDL::ObservableArray>& adopted_style_sheets, u32 index, JS::Value& value) -> WebIDL::ExceptionOr<void> {
        auto style_sheet = CSS::css_style_sheet_from_value(value);
        VERIFY(style_sheet);

        // Adopted sheets are not in any StyleSheetList, so nothing else announces them to the
        // engine. Observable array operations can write displaced entries before the insertion
        // point, so preserve their final order by attaching before the entry currently following
        // this index.
        if (auto last_index = last_index_of(adopted_style_sheets, *style_sheet, index); last_index.has_value() && *last_index > index)
            return {};
        CSS::record_stylesheet_attached(*style_sheet, document_or_shadow_root, first_sheet_after(adopted_style_sheets, index, index));
        CSS::Invalidation::invalidate_style_after_adopting_style_sheet(document_or_shadow_root, *style_sheet);
        return {};
    }));
    adopted_style_sheets->set_on_delete_an_indexed_value_callback(GC::create_function(realm.heap(), GC::captures(document_or_shadow_root, adopted_style_sheets), [](Node& document_or_shadow_root, GC::Ref<WebIDL::ObservableArray>& adopted_style_sheets, JS::Value value, u32 index) -> WebIDL::ExceptionOr<void> {
        // NB: Object.defineProperty() bypasses the set callback, so script can put any value in the array, including a
        //     sheet that the list could not have adopted, such as the sheet of a style element. Its attachment is not
        //     the list's to change.
        auto* style_sheet = CSS::css_style_sheet_from_value(value);
        if (!style_sheet || !style_sheet->constructed() || style_sheet->constructor_document().ptr() != &document_or_shadow_root.document())
            return {};

        // NB: The deleted value is still at index.
        auto last_index = last_index_of(adopted_style_sheets, *style_sheet, index);
        if (!last_index.has_value()) {
            CSS::record_stylesheet_detached(*style_sheet, document_or_shadow_root);
            CSS::Invalidation::invalidate_style_after_removing_adopted_style_sheet(document_or_shadow_root, *style_sheet);
            return {};
        }
        if (*last_index > index)
            return {};
        CSS::record_stylesheet_attached(*style_sheet, document_or_shadow_root, first_sheet_after(adopted_style_sheets, *last_index, index));
        CSS::Invalidation::invalidate_style_after_adopting_style_sheet(document_or_shadow_root, *style_sheet);
        return {};
    }));

    return adopted_style_sheets;
}

void for_each_adopted_style_sheet(WebIDL::ObservableArray& adopted_style_sheets, Function<void(CSS::StyleSheetState&)> const& callback)
{
    adopted_style_sheets.for_each_element_value([&](JS::Value value) {
        if (auto style_sheet = CSS::css_style_sheet_from_value(value))
            callback(*style_sheet);
    });
}

}
