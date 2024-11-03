/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Variant.h>
#include <LibWeb/ARIA/StateAndProperties.h>

namespace Web::ARIA {

ErrorOr<String> state_or_property_to_string_value(StateAndProperties state_or_property, AriaData const& aria_data, DefaultValueType default_value)
{
    switch (state_or_property) {
    case StateAndProperties::AriaActiveDescendant: {
        return aria_data.aria_active_descendant_or_default().value_or(String {});
    }
    case StateAndProperties::AriaAtomic: {
        bool value;
        if (default_value.has<bool>())
            value = aria_data.aria_atomic_or_default(default_value.get<bool>());
        else
            value = aria_data.aria_atomic_or_default();
        return value ? "true"_string : "false"_string;
    }
    case StateAndProperties::AriaAutoComplete: {
        auto value = aria_data.aria_auto_complete_or_default();
        switch (value) {
        case AriaAutocomplete::None:
            return "none"_string;
        case AriaAutocomplete::List:
            return "list"_string;
        case AriaAutocomplete::Both:
            return "both"_string;
        case AriaAutocomplete::Inline:
            return "inline"_string;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaBrailleLabel:
        return aria_data.aria_braille_label_or_default();
    case StateAndProperties::AriaBrailleRoleDescription:
        return aria_data.aria_braille_role_description_or_default();
    case StateAndProperties::AriaBusy:
        return String::from_utf8(aria_data.aria_busy_or_default() ? "true"sv : "false"sv);
    case StateAndProperties::AriaChecked:
        return ARIA::tristate_to_string(aria_data.aria_checked_or_default());
    case StateAndProperties::AriaColCount:
        return ARIA::optional_integer_to_string(aria_data.aria_col_count_or_default());
    case StateAndProperties::AriaColIndex:
        return ARIA::optional_integer_to_string(aria_data.aria_col_index_or_default());
    case StateAndProperties::AriaColIndexText:
        return aria_data.aria_col_index_text_or_default();
    case StateAndProperties::AriaColSpan:
        return ARIA::optional_integer_to_string(aria_data.aria_col_span_or_default());
    case StateAndProperties::AriaControls:
        return id_reference_list_to_string(aria_data.aria_controls_or_default());
    case StateAndProperties::AriaCurrent: {
        auto value = aria_data.aria_current_or_default();
        switch (value) {
        case AriaCurrent::False:
            return "false"_string;
        case AriaCurrent::True:
            return "true"_string;
        case AriaCurrent::Date:
            return "date"_string;
        case AriaCurrent::Location:
            return "location"_string;
        case AriaCurrent::Page:
            return "page"_string;
        case AriaCurrent::Step:
            return "step"_string;
        case AriaCurrent::Time:
            return "time"_string;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaDescribedBy:
        return id_reference_list_to_string(aria_data.aria_described_by_or_default());
    case StateAndProperties::AriaDescription:
        return aria_data.aria_description_or_default();
    case StateAndProperties::AriaDetails: {
        return aria_data.aria_details_or_default().value_or(String {});
    }
    case StateAndProperties::AriaDisabled:
        return aria_data.aria_disabled_or_default() ? "true"_string : "false"_string;
    case StateAndProperties::AriaDropEffect: {
        StringBuilder builder;
        auto value = aria_data.aria_drop_effect_or_default();
        for (auto const drop_effect : value) {
            StringView to_add;
            switch (drop_effect) {
            case AriaDropEffect::Copy:
                to_add = "copy"sv;
                break;
            case AriaDropEffect::Execute:
                to_add = "execute"sv;
                break;
            case AriaDropEffect::Link:
                to_add = "link"sv;
                break;
            case AriaDropEffect::Move:
                to_add = "move"sv;
                break;
            case AriaDropEffect::None:
                to_add = "none"sv;
                break;
            case AriaDropEffect::Popup:
                to_add = "popup"sv;
                break;
            }
            if (builder.is_empty())
                builder.append(to_add);
            else {
                builder.append(" "sv);
                builder.append(to_add);
            }
        }
        return builder.to_string();
    }
    case StateAndProperties::AriaErrorMessage: {
        return aria_data.aria_error_message_or_default().value_or(String {});
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
            return "false"_string;
        case AriaHasPopup::True:
            return "true"_string;
        case AriaHasPopup::Menu:
            return "menu"_string;
        case AriaHasPopup::Listbox:
            return "listbox"_string;
        case AriaHasPopup::Tree:
            return "tree"_string;
        case AriaHasPopup::Grid:
            return "grid"_string;
        case AriaHasPopup::Dialog:
            return "dialog"_string;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaHidden:
        return ARIA::optional_bool_to_string(aria_data.aria_hidden_or_default());
    case StateAndProperties::AriaInvalid: {
        auto value = aria_data.aria_invalid_or_default();
        switch (value) {
        case AriaInvalid::Grammar:
            return "grammar"_string;
        case AriaInvalid::False:
            return "false"_string;
        case AriaInvalid::Spelling:
            return "spelling"_string;
        case AriaInvalid::True:
            return "true"_string;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaKeyShortcuts:
        return aria_data.aria_key_shortcuts_or_default();
    case StateAndProperties::AriaLabel:
        return aria_data.aria_label_or_default();
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
            return "assertive"_string;
        case AriaLive::Off:
            return "off"_string;
        case AriaLive::Polite:
            return "polite"_string;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaModal:
        return aria_data.aria_modal_or_default() ? "true"_string : "false"_string;
    case StateAndProperties::AriaMultiLine:
        return aria_data.aria_multi_line_or_default() ? "true"_string : "false"_string;
    case StateAndProperties::AriaMultiSelectable:
        return aria_data.aria_multi_selectable_or_default() ? "true"_string : "false"_string;
    case StateAndProperties::AriaOrientation: {
        AriaOrientation value;
        if (default_value.has<AriaOrientation>())
            value = aria_data.aria_orientation_or_default(default_value.get<AriaOrientation>());
        else
            value = aria_data.aria_orientation_or_default();

        switch (value) {
        case AriaOrientation::Horizontal:
            return "horizontal"_string;
        case AriaOrientation::Undefined:
            return "undefined"_string;
        case AriaOrientation::Vertical:
            return "vertical"_string;
        }
        VERIFY_NOT_REACHED();
    }
    case StateAndProperties::AriaOwns:
        return id_reference_list_to_string(aria_data.aria_owns_or_default());
    case StateAndProperties::AriaPlaceholder:
        return aria_data.aria_placeholder_or_default();
    case StateAndProperties::AriaPosInSet:
        return ARIA::optional_integer_to_string(aria_data.aria_pos_in_set_or_default());
    case StateAndProperties::AriaPressed:
        return ARIA::tristate_to_string(aria_data.aria_pressed_or_default());
    case StateAndProperties::AriaReadOnly:
        return aria_data.aria_read_only_or_default() ? "true"_string : "false"_string;
    case StateAndProperties::AriaRelevant: {
        StringBuilder builder;
        auto value = aria_data.aria_relevant_or_default();
        for (auto const relevant : value) {
            StringView to_add;
            switch (relevant) {
            case AriaRelevant::Additions:
                to_add = "additions"sv;
                break;
            case AriaRelevant::AdditionsText:
                to_add = "additions text"sv;
                break;
            case AriaRelevant::All:
                to_add = "all"sv;
                break;
            case AriaRelevant::Removals:
                to_add = "removals"sv;
                break;
            case AriaRelevant::Text:
                to_add = "text"sv;
                break;
            }
            if (builder.is_empty())
                builder.append(to_add);
            else {
                builder.append(" "sv);
                builder.append(to_add);
            }
        }
        return builder.to_string();
    }
    case StateAndProperties::AriaRequired:
        return String::from_utf8(aria_data.aria_required_or_default() ? "true"sv : "false"sv);
    case StateAndProperties::AriaRoleDescription:
        return aria_data.aria_role_description_or_default();
    case StateAndProperties::AriaRowCount:
        return ARIA::optional_integer_to_string(aria_data.aria_row_count_or_default());
    case StateAndProperties::AriaRowIndex:
        return ARIA::optional_integer_to_string(aria_data.aria_row_index_or_default());
    case StateAndProperties::AriaRowIndexText:
        return aria_data.aria_row_index_text_or_default();
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
            return "ascending"_string;
        case AriaSort::Descending:
            return "descending"_string;
        case AriaSort::None:
            return "none"_string;
        case AriaSort::Other:
            return "other"_string;
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
        return aria_data.aria_value_text_or_default();
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<String> tristate_to_string(Tristate value)
{
    switch (value) {
    case Tristate::False:
        return "false"_string;
    case Tristate::True:
        return "true"_string;
    case Tristate::Undefined:
        return "undefined"_string;
    case Tristate::Mixed:
        return "mixed"_string;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<String> optional_integer_to_string(Optional<i32> value)
{
    if (value.has_value())
        return String::number(value.value());
    return String {};
}

ErrorOr<String> optional_bool_to_string(Optional<bool> value)
{
    if (!value.has_value())
        return "undefined"_string;
    if (value.value())
        return "true"_string;
    return "false"_string;
}

ErrorOr<String> optional_number_to_string(Optional<f64> value)
{
    if (!value.has_value())
        return "undefined"_string;
    return String::number(value.value());
}

ErrorOr<String> id_reference_list_to_string(Vector<String> const& value)
{
    StringBuilder builder;
    for (auto const& id : value) {
        if (builder.is_empty()) {
            builder.append(id);
        } else {
            builder.append(" "sv);
            builder.append(id);
        }
    }
    return builder.to_string();
}

StringView state_or_property_to_string(StateAndProperties value)
{
    switch (value) {
    case StateAndProperties::AriaActiveDescendant:
        return "aria-activedescendant"sv;
    case StateAndProperties::AriaAtomic:
        return "aria-atomic"sv;
    case StateAndProperties::AriaAutoComplete:
        return "aria-autocomplete"sv;
    case StateAndProperties::AriaBrailleLabel:
        return "aria-braillelabel"sv;
    case StateAndProperties::AriaBrailleRoleDescription:
        return "aria-brailleroledescription"sv;
    case StateAndProperties::AriaBusy:
        return "aria-busy"sv;
    case StateAndProperties::AriaChecked:
        return "aria-checked"sv;
    case StateAndProperties::AriaColCount:
        return "aria-colcount"sv;
    case StateAndProperties::AriaColIndex:
        return "aria-colindex"sv;
    case StateAndProperties::AriaColIndexText:
        return "aria-colindextext"sv;
    case StateAndProperties::AriaColSpan:
        return "aria-colspan"sv;
    case StateAndProperties::AriaControls:
        return "aria-controls"sv;
    case StateAndProperties::AriaCurrent:
        return "aria-current"sv;
    case StateAndProperties::AriaDescribedBy:
        return "aria-describedby"sv;
    case StateAndProperties::AriaDescription:
        return "aria-description"sv;
    case StateAndProperties::AriaDetails:
        return "aria-details"sv;
    case StateAndProperties::AriaDisabled:
        return "aria-disabled"sv;
    case StateAndProperties::AriaDropEffect:
        return "aria-dropeffect"sv;
    case StateAndProperties::AriaErrorMessage:
        return "aria-errormessage"sv;
    case StateAndProperties::AriaExpanded:
        return "aria-expanded"sv;
    case StateAndProperties::AriaFlowTo:
        return "aria-flowto"sv;
    case StateAndProperties::AriaGrabbed:
        return "aria-grabbed"sv;
    case StateAndProperties::AriaHasPopup:
        return "aria-haspopup"sv;
    case StateAndProperties::AriaHidden:
        return "aria-hidden"sv;
    case StateAndProperties::AriaInvalid:
        return "aria-invalid"sv;
    case StateAndProperties::AriaKeyShortcuts:
        return "aria-keyshortcuts"sv;
    case StateAndProperties::AriaLabel:
        return "aria-label"sv;
    case StateAndProperties::AriaLabelledBy:
        return "aria-labelledby"sv;
    case StateAndProperties::AriaLevel:
        return "aria-level"sv;
    case StateAndProperties::AriaLive:
        return "aria-live"sv;
    case StateAndProperties::AriaModal:
        return "aria-modal"sv;
    case StateAndProperties::AriaMultiLine:
        return "aria-multiline"sv;
    case StateAndProperties::AriaMultiSelectable:
        return "aria-multiselectable"sv;
    case StateAndProperties::AriaOrientation:
        return "aria-orientation"sv;
    case StateAndProperties::AriaOwns:
        return "aria-owns"sv;
    case StateAndProperties::AriaPlaceholder:
        return "aria-placeholder"sv;
    case StateAndProperties::AriaPosInSet:
        return "aria-posinset"sv;
    case StateAndProperties::AriaPressed:
        return "aria-pressed"sv;
    case StateAndProperties::AriaReadOnly:
        return "aria-readonly"sv;
    case StateAndProperties::AriaRelevant:
        return "aria-relevant"sv;
    case StateAndProperties::AriaRequired:
        return "aria-required"sv;
    case StateAndProperties::AriaRoleDescription:
        return "aria-roledescription"sv;
    case StateAndProperties::AriaRowCount:
        return "aria-rowcount"sv;
    case StateAndProperties::AriaRowIndex:
        return "aria-rowindex"sv;
    case StateAndProperties::AriaRowIndexText:
        return "aria-rowindextext"sv;
    case StateAndProperties::AriaRowSpan:
        return "aria-rowspan"sv;
    case StateAndProperties::AriaSelected:
        return "aria-selected"sv;
    case StateAndProperties::AriaSetSize:
        return "aria-setsize"sv;
    case StateAndProperties::AriaSort:
        return "aria-sort"sv;
    case StateAndProperties::AriaValueMax:
        return "aria-valuemax"sv;
    case StateAndProperties::AriaValueMin:
        return "aria-valuemin"sv;
    case StateAndProperties::AriaValueNow:
        return "aria-valuenow"sv;
    case StateAndProperties::AriaValueText:
        return "aria-valuetext"sv;
    }
    VERIFY_NOT_REACHED();
}

}
