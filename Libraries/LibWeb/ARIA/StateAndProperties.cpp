/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <AK/Variant.h>
#include <LibWeb/ARIA/StateAndProperties.h>

namespace Web::ARIA {

template<size_t length>
static constexpr Utf16View utf16_view(char16_t const (&string)[length])
{
    return { string, length - 1 };
}

static Utf16String serialize_aria_string(Utf16String const& value)
{
    return value;
}

ErrorOr<Utf16String> state_or_property_to_string_value(StateAndProperties state_or_property, AriaData const& aria_data, DefaultValueType default_value)
{
    switch (state_or_property) {
    case StateAndProperties::AriaActiveDescendant: {
        return serialize_aria_string(aria_data.aria_active_descendant_or_default().value_or(Utf16String {}));
    }
    case StateAndProperties::AriaAtomic: {
        bool value;
        if (default_value.has<bool>())
            value = aria_data.aria_atomic_or_default(default_value.get<bool>());
        else
            value = aria_data.aria_atomic_or_default();
        return value ? "true"_utf16 : "false"_utf16;
    }
    case StateAndProperties::AriaAutoComplete: {
        auto value = aria_data.aria_auto_complete_or_default();
        switch (value) {
        case AriaAutocomplete::None:
            return "none"_utf16;
        case AriaAutocomplete::List:
            return "list"_utf16;
        case AriaAutocomplete::Both:
            return "both"_utf16;
        case AriaAutocomplete::Inline:
            return "inline"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaBrailleLabel:
        return serialize_aria_string(aria_data.aria_braille_label_or_default());
    case StateAndProperties::AriaBrailleRoleDescription:
        return serialize_aria_string(aria_data.aria_braille_role_description_or_default());
    case StateAndProperties::AriaBusy:
        return aria_data.aria_busy_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaChecked:
        return ARIA::tristate_to_string(aria_data.aria_checked_or_default());
    case StateAndProperties::AriaColCount:
        return ARIA::optional_integer_to_string(aria_data.aria_col_count_or_default());
    case StateAndProperties::AriaColIndex:
        return ARIA::optional_integer_to_string(aria_data.aria_col_index_or_default());
    case StateAndProperties::AriaColIndexText:
        return serialize_aria_string(aria_data.aria_col_index_text_or_default());
    case StateAndProperties::AriaColSpan:
        return ARIA::optional_integer_to_string(aria_data.aria_col_span_or_default());
    case StateAndProperties::AriaControls:
        return id_reference_list_to_string(aria_data.aria_controls_or_default());
    case StateAndProperties::AriaCurrent: {
        auto value = aria_data.aria_current_or_default();
        switch (value) {
        case AriaCurrent::False:
            return "false"_utf16;
        case AriaCurrent::True:
            return "true"_utf16;
        case AriaCurrent::Date:
            return "date"_utf16;
        case AriaCurrent::Location:
            return "location"_utf16;
        case AriaCurrent::Page:
            return "page"_utf16;
        case AriaCurrent::Step:
            return "step"_utf16;
        case AriaCurrent::Time:
            return "time"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaDescribedBy:
        return id_reference_list_to_string(aria_data.aria_described_by_or_default());
    case StateAndProperties::AriaDescription:
        return serialize_aria_string(aria_data.aria_description_or_default());
    case StateAndProperties::AriaDetails: {
        return serialize_aria_string(aria_data.aria_details_or_default().value_or(Utf16String {}));
    }
    case StateAndProperties::AriaDisabled:
        return aria_data.aria_disabled_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaDropEffect: {
        Utf16StringBuilder builder;
        auto value = aria_data.aria_drop_effect_or_default();
        for (auto const drop_effect : value) {
            Utf16View to_add;
            switch (drop_effect) {
            case AriaDropEffect::Copy:
                to_add = "copy"_utf16;
                break;
            case AriaDropEffect::Execute:
                to_add = "execute"_utf16;
                break;
            case AriaDropEffect::Link:
                to_add = "link"_utf16;
                break;
            case AriaDropEffect::Move:
                to_add = "move"_utf16;
                break;
            case AriaDropEffect::None:
                to_add = "none"_utf16;
                break;
            case AriaDropEffect::Popup:
                to_add = "popup"_utf16;
                break;
            }
            if (builder.is_empty())
                builder.append(to_add);
            else {
                builder.append_ascii(' ');
                builder.append(to_add);
            }
        }
        return builder.to_string();
    }
    case StateAndProperties::AriaErrorMessage: {
        return serialize_aria_string(aria_data.aria_error_message_or_default().value_or(Utf16String {}));
    }
    case StateAndProperties::AriaExpanded:
        return ARIA::optional_bool_to_string(aria_data.aria_expanded_or_default());
    case StateAndProperties::AriaFlowTo:
        return id_reference_list_to_string(aria_data.aria_flow_to_or_default());
    case StateAndProperties::AriaGrabbed:
        return ARIA::optional_bool_to_string(aria_data.aria_grabbed_or_default());
    case StateAndProperties::AriaHasPopup: {
        auto value = aria_data.aria_has_popup_or_default();
        switch (value) {
        case AriaHasPopup::False:
            return "false"_utf16;
        case AriaHasPopup::True:
            return "true"_utf16;
        case AriaHasPopup::Menu:
            return "menu"_utf16;
        case AriaHasPopup::Listbox:
            return "listbox"_utf16;
        case AriaHasPopup::Tree:
            return "tree"_utf16;
        case AriaHasPopup::Grid:
            return "grid"_utf16;
        case AriaHasPopup::Dialog:
            return "dialog"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaHidden:
        return ARIA::optional_bool_to_string(aria_data.aria_hidden_or_default());
    case StateAndProperties::AriaInvalid: {
        auto value = aria_data.aria_invalid_or_default();
        switch (value) {
        case AriaInvalid::Grammar:
            return "grammar"_utf16;
        case AriaInvalid::False:
            return "false"_utf16;
        case AriaInvalid::Spelling:
            return "spelling"_utf16;
        case AriaInvalid::True:
            return "true"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaKeyShortcuts:
        return serialize_aria_string(aria_data.aria_key_shortcuts_or_default());
    case StateAndProperties::AriaLabel:
        return serialize_aria_string(aria_data.aria_label_or_default());
    case StateAndProperties::AriaLabelledBy:
        return id_reference_list_to_string(aria_data.aria_labelled_by_or_default());
    case StateAndProperties::AriaLevel:
        return ARIA::optional_integer_to_string(aria_data.aria_level_or_default());
    case StateAndProperties::AriaLive: {
        AriaLive value;
        if (default_value.has<AriaLive>())
            value = aria_data.aria_live_or_default(default_value.get<AriaLive>());
        else
            value = aria_data.aria_live_or_default();

        switch (value) {
        case AriaLive::Assertive:
            return "assertive"_utf16;
        case AriaLive::Off:
            return "off"_utf16;
        case AriaLive::Polite:
            return "polite"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaModal:
        return aria_data.aria_modal_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaMultiLine:
        return aria_data.aria_multi_line_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaMultiSelectable:
        return aria_data.aria_multi_selectable_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaOrientation: {
        AriaOrientation value;
        if (default_value.has<AriaOrientation>())
            value = aria_data.aria_orientation_or_default(default_value.get<AriaOrientation>());
        else
            value = aria_data.aria_orientation_or_default();

        switch (value) {
        case AriaOrientation::Horizontal:
            return "horizontal"_utf16;
        case AriaOrientation::Undefined:
            return "undefined"_utf16;
        case AriaOrientation::Vertical:
            return "vertical"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaOwns:
        return id_reference_list_to_string(aria_data.aria_owns_or_default());
    case StateAndProperties::AriaPlaceholder:
        return serialize_aria_string(aria_data.aria_placeholder_or_default());
    case StateAndProperties::AriaPosInSet:
        return ARIA::optional_integer_to_string(aria_data.aria_pos_in_set_or_default());
    case StateAndProperties::AriaPressed:
        return ARIA::tristate_to_string(aria_data.aria_pressed_or_default());
    case StateAndProperties::AriaReadOnly:
        return aria_data.aria_read_only_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaRelevant: {
        Utf16StringBuilder builder;
        auto value = aria_data.aria_relevant_or_default();
        for (auto const relevant : value) {
            Utf16View to_add;
            switch (relevant) {
            case AriaRelevant::Additions:
                to_add = "additions"_utf16;
                break;
            case AriaRelevant::AdditionsText:
                to_add = "additions text"_utf16;
                break;
            case AriaRelevant::All:
                to_add = "all"_utf16;
                break;
            case AriaRelevant::Removals:
                to_add = "removals"_utf16;
                break;
            case AriaRelevant::Text:
                to_add = "text"_utf16;
                break;
            }
            if (builder.is_empty())
                builder.append(to_add);
            else {
                builder.append_ascii(' ');
                builder.append(to_add);
            }
        }
        return builder.to_string();
    }
    case StateAndProperties::AriaRequired:
        return aria_data.aria_required_or_default() ? "true"_utf16 : "false"_utf16;
    case StateAndProperties::AriaRoleDescription:
        return serialize_aria_string(aria_data.aria_role_description_or_default());
    case StateAndProperties::AriaRowCount:
        return ARIA::optional_integer_to_string(aria_data.aria_row_count_or_default());
    case StateAndProperties::AriaRowIndex:
        return ARIA::optional_integer_to_string(aria_data.aria_row_index_or_default());
    case StateAndProperties::AriaRowIndexText:
        return serialize_aria_string(aria_data.aria_row_index_text_or_default());
    case StateAndProperties::AriaRowSpan:
        return ARIA::optional_integer_to_string(aria_data.aria_row_span_or_default());
    case StateAndProperties::AriaSelected:
        return ARIA::optional_bool_to_string(aria_data.aria_selected_or_default());
    case StateAndProperties::AriaSetSize:
        return ARIA::optional_integer_to_string(aria_data.aria_set_size_or_default());
    case StateAndProperties::AriaSort: {
        auto value = aria_data.aria_sort_or_default();
        switch (value) {
        case AriaSort::Ascending:
            return "ascending"_utf16;
        case AriaSort::Descending:
            return "descending"_utf16;
        case AriaSort::None:
            return "none"_utf16;
        case AriaSort::Other:
            return "other"_utf16;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaValueMax:
        if (default_value.has<f64>())
            return ARIA::optional_number_to_string(aria_data.aria_value_max_or_default(default_value.get<f64>()));
        else
            return ARIA::optional_number_to_string(aria_data.aria_value_max_or_default());
    case StateAndProperties::AriaValueMin:
        if (default_value.has<f64>())
            return ARIA::optional_number_to_string(aria_data.aria_value_min_or_default(default_value.get<f64>()));
        else
            return ARIA::optional_number_to_string(aria_data.aria_value_min_or_default());
    case StateAndProperties::AriaValueNow:
        return ARIA::optional_number_to_string(aria_data.aria_value_now_or_default());
    case StateAndProperties::AriaValueText:
        return serialize_aria_string(aria_data.aria_value_text_or_default());
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Utf16String> tristate_to_string(Tristate value)
{
    switch (value) {
    case Tristate::False:
        return "false"_utf16;
    case Tristate::True:
        return "true"_utf16;
    case Tristate::Undefined:
        return "undefined"_utf16;
    case Tristate::Mixed:
        return "mixed"_utf16;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Utf16String> optional_integer_to_string(Optional<i32> value)
{
    if (value.has_value())
        return Utf16String::number(value.value());
    return Utf16String {};
}

ErrorOr<Utf16String> optional_bool_to_string(Optional<bool> value)
{
    if (!value.has_value())
        return "undefined"_utf16;
    if (value.value())
        return "true"_utf16;
    return "false"_utf16;
}

ErrorOr<Utf16String> optional_number_to_string(Optional<f64> value)
{
    if (!value.has_value())
        return "undefined"_utf16;
    return Utf16String::number(value.value());
}

ErrorOr<Utf16String> id_reference_list_to_string(Vector<Utf16String> const& value)
{
    Utf16StringBuilder builder;
    for (auto const& id : value) {
        auto serialized_id = serialize_aria_string(id);
        if (builder.is_empty()) {
            builder.append(serialized_id);
        } else {
            builder.append_ascii(' ');
            builder.append(serialized_id);
        }
    }
    return builder.to_string();
}

Utf16View state_or_property_to_string(StateAndProperties value)
{
    switch (value) {
    case StateAndProperties::AriaActiveDescendant:
        return utf16_view(u"aria-activedescendant");
    case StateAndProperties::AriaAtomic:
        return utf16_view(u"aria-atomic");
    case StateAndProperties::AriaAutoComplete:
        return utf16_view(u"aria-autocomplete");
    case StateAndProperties::AriaBrailleLabel:
        return utf16_view(u"aria-braillelabel");
    case StateAndProperties::AriaBrailleRoleDescription:
        return utf16_view(u"aria-brailleroledescription");
    case StateAndProperties::AriaBusy:
        return utf16_view(u"aria-busy");
    case StateAndProperties::AriaChecked:
        return utf16_view(u"aria-checked");
    case StateAndProperties::AriaColCount:
        return utf16_view(u"aria-colcount");
    case StateAndProperties::AriaColIndex:
        return utf16_view(u"aria-colindex");
    case StateAndProperties::AriaColIndexText:
        return utf16_view(u"aria-colindextext");
    case StateAndProperties::AriaColSpan:
        return utf16_view(u"aria-colspan");
    case StateAndProperties::AriaControls:
        return utf16_view(u"aria-controls");
    case StateAndProperties::AriaCurrent:
        return utf16_view(u"aria-current");
    case StateAndProperties::AriaDescribedBy:
        return utf16_view(u"aria-describedby");
    case StateAndProperties::AriaDescription:
        return utf16_view(u"aria-description");
    case StateAndProperties::AriaDetails:
        return utf16_view(u"aria-details");
    case StateAndProperties::AriaDisabled:
        return utf16_view(u"aria-disabled");
    case StateAndProperties::AriaDropEffect:
        return utf16_view(u"aria-dropeffect");
    case StateAndProperties::AriaErrorMessage:
        return utf16_view(u"aria-errormessage");
    case StateAndProperties::AriaExpanded:
        return utf16_view(u"aria-expanded");
    case StateAndProperties::AriaFlowTo:
        return utf16_view(u"aria-flowto");
    case StateAndProperties::AriaGrabbed:
        return utf16_view(u"aria-grabbed");
    case StateAndProperties::AriaHasPopup:
        return utf16_view(u"aria-haspopup");
    case StateAndProperties::AriaHidden:
        return utf16_view(u"aria-hidden");
    case StateAndProperties::AriaInvalid:
        return utf16_view(u"aria-invalid");
    case StateAndProperties::AriaKeyShortcuts:
        return utf16_view(u"aria-keyshortcuts");
    case StateAndProperties::AriaLabel:
        return utf16_view(u"aria-label");
    case StateAndProperties::AriaLabelledBy:
        return utf16_view(u"aria-labelledby");
    case StateAndProperties::AriaLevel:
        return utf16_view(u"aria-level");
    case StateAndProperties::AriaLive:
        return utf16_view(u"aria-live");
    case StateAndProperties::AriaModal:
        return utf16_view(u"aria-modal");
    case StateAndProperties::AriaMultiLine:
        return utf16_view(u"aria-multiline");
    case StateAndProperties::AriaMultiSelectable:
        return utf16_view(u"aria-multiselectable");
    case StateAndProperties::AriaOrientation:
        return utf16_view(u"aria-orientation");
    case StateAndProperties::AriaOwns:
        return utf16_view(u"aria-owns");
    case StateAndProperties::AriaPlaceholder:
        return utf16_view(u"aria-placeholder");
    case StateAndProperties::AriaPosInSet:
        return utf16_view(u"aria-posinset");
    case StateAndProperties::AriaPressed:
        return utf16_view(u"aria-pressed");
    case StateAndProperties::AriaReadOnly:
        return utf16_view(u"aria-readonly");
    case StateAndProperties::AriaRelevant:
        return utf16_view(u"aria-relevant");
    case StateAndProperties::AriaRequired:
        return utf16_view(u"aria-required");
    case StateAndProperties::AriaRoleDescription:
        return utf16_view(u"aria-roledescription");
    case StateAndProperties::AriaRowCount:
        return utf16_view(u"aria-rowcount");
    case StateAndProperties::AriaRowIndex:
        return utf16_view(u"aria-rowindex");
    case StateAndProperties::AriaRowIndexText:
        return utf16_view(u"aria-rowindextext");
    case StateAndProperties::AriaRowSpan:
        return utf16_view(u"aria-rowspan");
    case StateAndProperties::AriaSelected:
        return utf16_view(u"aria-selected");
    case StateAndProperties::AriaSetSize:
        return utf16_view(u"aria-setsize");
    case StateAndProperties::AriaSort:
        return utf16_view(u"aria-sort");
    case StateAndProperties::AriaValueMax:
        return utf16_view(u"aria-valuemax");
    case StateAndProperties::AriaValueMin:
        return utf16_view(u"aria-valuemin");
    case StateAndProperties::AriaValueNow:
        return utf16_view(u"aria-valuenow");
    case StateAndProperties::AriaValueText:
        return utf16_view(u"aria-valuetext");
    }
    VERIFY_NOT_REACHED();
}

}
