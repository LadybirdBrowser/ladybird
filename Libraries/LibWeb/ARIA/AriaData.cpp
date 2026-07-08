/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::ARIA {

AriaData::AriaData(Web::ARIA::ARIAMixin const& source)
{
    m_aria_active_descendant = source.aria_active_descendant();
    m_aria_atomic = AriaData::parse_optional_true_false(source.aria_atomic());
    m_aria_auto_complete = AriaData::parse_aria_autocomplete(source.aria_auto_complete());
    m_aria_braille_label = source.aria_braille_label().value_or(Utf16String {});
    m_aria_braille_role_description = source.aria_braille_role_description().value_or(Utf16String {});
    m_aria_busy = AriaData::parse_true_false(source.aria_busy());
    m_aria_checked = AriaData::parse_tristate(source.aria_checked());
    m_aria_col_count = AriaData::parse_integer(source.aria_col_count());
    m_aria_col_index = AriaData::parse_integer(source.aria_col_index());
    m_aria_col_index_text = source.aria_col_index_text().value_or(Utf16String {});
    m_aria_col_span = AriaData::parse_integer(source.aria_col_span());
    m_aria_controls = source.parse_id_reference_list(source.aria_controls());
    m_aria_current = AriaData::parse_aria_current(source.aria_current());
    m_aria_described_by = source.parse_id_reference_list(source.aria_described_by());
    m_aria_description = source.aria_description().value_or(Utf16String {});
    m_aria_details = source.parse_id_reference(source.aria_details());
    m_aria_disabled = AriaData::parse_true_false(source.aria_disabled());
    m_aria_drop_effect = AriaData::parse_aria_drop_effect(source.aria_drop_effect());
    m_aria_error_message = source.parse_id_reference(source.aria_error_message());
    m_aria_expanded = AriaData::parse_true_false_undefined(source.aria_expanded());
    m_aria_flow_to = source.parse_id_reference_list(source.aria_flow_to());
    m_aria_grabbed = AriaData::parse_true_false_undefined(source.aria_grabbed());
    m_aria_has_popup = AriaData::parse_aria_has_popup(source.aria_has_popup());
    m_aria_hidden = AriaData::parse_true_false_undefined(source.aria_hidden());
    m_aria_invalid = AriaData::parse_aria_invalid(source.aria_invalid());
    m_aria_key_shortcuts = source.aria_key_shortcuts().value_or(Utf16String {});
    m_aria_label = source.aria_label().value_or(Utf16String {});
    m_aria_labelled_by = source.parse_id_reference_list(source.aria_labelled_by());
    m_aria_level = AriaData::parse_integer(source.aria_level());
    m_aria_live = AriaData::parse_aria_live(source.aria_live());
    m_aria_modal = AriaData::parse_true_false(source.aria_modal());
    m_aria_multi_line = AriaData::parse_true_false(source.aria_multi_line());
    m_aria_multi_selectable = AriaData::parse_true_false(source.aria_multi_selectable());
    m_aria_orientation = AriaData::parse_aria_orientation(source.aria_orientation());
    m_aria_owns = source.parse_id_reference_list(source.aria_owns());
    m_aria_placeholder = source.aria_placeholder().value_or(Utf16String {});
    m_aria_pos_in_set = AriaData::parse_integer(source.aria_pos_in_set());
    m_aria_pressed = AriaData::parse_tristate(source.aria_pressed());
    m_aria_read_only = AriaData::parse_true_false(source.aria_read_only());
    m_aria_relevant = AriaData::parse_aria_relevant(source.aria_relevant());
    m_aria_required = AriaData::parse_true_false(source.aria_required());
    m_aria_role_description = source.aria_role_description().value_or(Utf16String {});
    m_aria_row_count = AriaData::parse_integer(source.aria_row_count());
    m_aria_row_index = AriaData::parse_integer(source.aria_row_index());
    m_aria_row_index_text = source.aria_row_index_text().value_or(Utf16String {});
    m_aria_row_span = AriaData::parse_integer(source.aria_row_span());
    m_aria_selected = AriaData::parse_true_false_undefined(source.aria_selected());
    m_aria_set_size = AriaData::parse_integer(source.aria_set_size());
    m_aria_sort = AriaData::parse_aria_sort(source.aria_sort());
    m_aria_value_max = AriaData::parse_number(source.aria_value_max());
    m_aria_value_min = AriaData::parse_number(source.aria_value_min());
    m_aria_value_now = AriaData::parse_number(source.aria_value_now());
    m_aria_value_text = source.aria_value_text().value_or(Utf16String {});
}

static bool value_equals(Optional<Utf16String> const& value, Utf16View expected)
{
    return value.has_value() && value->utf16_view() == expected;
}

static void for_each_ascii_whitespace_separated_token(Utf16View input, Function<IterationDecision(Utf16View)> const& callback)
{
    size_t start = 0;
    for (size_t i = 0; i <= input.length_in_code_units(); ++i) {
        if (i != input.length_in_code_units() && !Infra::is_ascii_whitespace(input.code_unit_at(i)))
            continue;

        if (i > start && callback(input.substring_view(start, i - start)) == IterationDecision::Break)
            return;
        start = i + 1;
    }
}

bool AriaData::parse_true_false(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"true"sv))
        return true;
    if (value_equals(value, u"false"sv))
        return false;
    return false;
}

Tristate AriaData::parse_tristate(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"true"sv))
        return Tristate::True;
    if (value_equals(value, u"false"sv))
        return Tristate::False;
    if (value_equals(value, u"mixed"sv))
        return Tristate::Mixed;
    if (value_equals(value, u"undefined"sv))
        return Tristate::Undefined;
    return Tristate::Undefined;
}

Optional<bool> AriaData::parse_true_false_undefined(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"true"sv))
        return true;
    if (value_equals(value, u"false"sv))
        return false;
    if (value_equals(value, u"undefined"sv))
        return {};
    return {};
}

Optional<i32> AriaData::parse_integer(Optional<Utf16String> const& value)
{
    if (!value.has_value())
        return {};
    return value->to_number<i32>();
}

Optional<f64> AriaData::parse_number(Optional<Utf16String> const& value)
{
    if (!value.has_value())
        return {};
    return value->to_number<double>(TrimWhitespace::Yes);
}

Optional<Utf16String> AriaData::aria_active_descendant_or_default() const
{
    return m_aria_active_descendant;
}

bool AriaData::aria_atomic_or_default(bool default_value) const
{
    auto value = m_aria_atomic;
    if (!value.has_value())
        return default_value;
    return value.value();
}

AriaAutocomplete AriaData::aria_auto_complete_or_default() const
{
    return m_aria_auto_complete;
}

Utf16String AriaData::aria_braille_label_or_default() const
{
    return m_aria_braille_label;
}

Utf16String AriaData::aria_braille_role_description_or_default() const
{
    return m_aria_braille_role_description;
}

bool AriaData::aria_busy_or_default() const
{
    return m_aria_busy;
}

Tristate AriaData::aria_checked_or_default() const
{
    return m_aria_checked;
}

Optional<i32> AriaData::aria_col_count_or_default() const
{
    return m_aria_col_count;
}

Optional<i32> AriaData::aria_col_index_or_default() const
{
    return m_aria_col_index;
}

Utf16String AriaData::aria_col_index_text_or_default() const
{
    return m_aria_col_index_text;
}

Optional<i32> AriaData::aria_col_span_or_default() const
{
    return m_aria_col_span;
}

Vector<Utf16String> AriaData::aria_controls_or_default() const
{
    return m_aria_controls;
}

AriaCurrent AriaData::aria_current_or_default() const
{
    return m_aria_current;
}

Vector<Utf16String> AriaData::aria_described_by_or_default() const
{
    return m_aria_described_by;
}

Utf16String AriaData::aria_description_or_default() const
{
    return m_aria_description;
}

Optional<Utf16String> AriaData::aria_details_or_default() const
{
    return m_aria_details;
}

bool AriaData::aria_disabled_or_default() const
{
    return m_aria_disabled;
}

Vector<AriaDropEffect> AriaData::aria_drop_effect_or_default() const
{
    return m_aria_drop_effect;
}

Optional<Utf16String> AriaData::aria_error_message_or_default() const
{
    return m_aria_error_message;
}

Optional<bool> AriaData::aria_expanded_or_default() const
{
    return m_aria_expanded;
}

Vector<Utf16String> AriaData::aria_flow_to_or_default() const
{
    return m_aria_flow_to;
}

Optional<bool> AriaData::aria_grabbed_or_default() const
{
    return m_aria_grabbed;
}

AriaHasPopup AriaData::aria_has_popup_or_default() const
{
    return m_aria_has_popup;
}

Optional<bool> AriaData::aria_hidden_or_default() const
{
    return m_aria_hidden;
}

AriaInvalid AriaData::aria_invalid_or_default() const
{
    return m_aria_invalid;
}

Utf16String AriaData::aria_key_shortcuts_or_default() const
{
    return m_aria_key_shortcuts;
}

Utf16String AriaData::aria_label_or_default() const
{
    return m_aria_label;
}

Vector<Utf16String> AriaData::aria_labelled_by_or_default() const
{
    return m_aria_labelled_by;
}

Optional<i32> AriaData::aria_level_or_default() const
{
    return m_aria_level;
}

AriaLive AriaData::aria_live_or_default(AriaLive default_value) const
{
    auto value = m_aria_live;
    if (!value.has_value())
        return default_value;

    return value.value();
}

bool AriaData::aria_modal_or_default() const
{
    return m_aria_modal;
}

bool AriaData::aria_multi_line_or_default() const
{
    return m_aria_multi_line;
}

bool AriaData::aria_multi_selectable_or_default() const
{
    return m_aria_multi_selectable;
}

AriaOrientation AriaData::aria_orientation_or_default(AriaOrientation default_value) const
{
    auto value = m_aria_orientation;
    if (!value.has_value())
        return default_value;

    return value.value();
}

Vector<Utf16String> AriaData::aria_owns_or_default() const
{
    return m_aria_owns;
}

Utf16String AriaData::aria_placeholder_or_default() const
{
    return m_aria_placeholder;
}

Optional<i32> AriaData::aria_pos_in_set_or_default() const
{
    return m_aria_pos_in_set;
}

Tristate AriaData::aria_pressed_or_default() const
{
    return m_aria_pressed;
}

bool AriaData::aria_read_only_or_default() const
{
    return m_aria_read_only;
}

Vector<AriaRelevant> AriaData::aria_relevant_or_default() const
{
    return m_aria_relevant;
}

bool AriaData::aria_required_or_default() const
{
    return m_aria_required;
}

Utf16String AriaData::aria_role_description_or_default() const
{
    return m_aria_role_description;
}

Optional<i32> AriaData::aria_row_count_or_default() const
{
    return m_aria_row_count;
}

Optional<i32> AriaData::aria_row_index_or_default() const
{
    return m_aria_row_index;
}

Utf16String AriaData::aria_row_index_text_or_default() const
{
    return m_aria_row_index_text;
}

Optional<i32> AriaData::aria_row_span_or_default() const
{
    return m_aria_row_span;
}

Optional<bool> AriaData::aria_selected_or_default() const
{
    return m_aria_selected;
}

Optional<i32> AriaData::aria_set_size_or_default() const
{
    return m_aria_set_size;
}

AriaSort AriaData::aria_sort_or_default() const
{
    return m_aria_sort;
}

Optional<f64> AriaData::aria_value_max_or_default(Optional<f64> default_value) const
{
    auto value = m_aria_value_max;
    if (!value.has_value())
        return default_value;
    return value;
}

Optional<f64> AriaData::aria_value_min_or_default(Optional<f64> default_value) const
{
    auto value = m_aria_value_min;
    if (!value.has_value())
        return default_value;
    return value;
}

Optional<f64> AriaData::aria_value_now_or_default() const
{
    return m_aria_value_now;
}

Utf16String AriaData::aria_value_text_or_default() const
{
    return m_aria_value_text;
}

AriaAutocomplete AriaData::parse_aria_autocomplete(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"inline"sv))
        return AriaAutocomplete::Inline;
    if (value_equals(value, u"list"sv))
        return AriaAutocomplete::List;
    if (value_equals(value, u"both"sv))
        return AriaAutocomplete::Both;
    if (value_equals(value, u"none"sv))
        return AriaAutocomplete::None;
    return AriaAutocomplete::None;
}

AriaCurrent AriaData::parse_aria_current(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"page"sv))
        return AriaCurrent::Page;
    if (value_equals(value, u"step"sv))
        return AriaCurrent::Step;
    if (value_equals(value, u"location"sv))
        return AriaCurrent::Location;
    if (value_equals(value, u"date"sv))
        return AriaCurrent::Date;
    if (value_equals(value, u"time"sv))
        return AriaCurrent::Time;
    if (value_equals(value, u"true"sv))
        return AriaCurrent::True;
    if (value_equals(value, u"false"sv))
        return AriaCurrent::False;
    return AriaCurrent::False;
}

Vector<AriaDropEffect> AriaData::parse_aria_drop_effect(Optional<Utf16String> const& value)
{
    if (!value.has_value())
        return {};

    Vector<AriaDropEffect> result;

    for_each_ascii_whitespace_separated_token(value->utf16_view(), [&](auto token) {
        if (token == u"copy"sv)
            result.append(AriaDropEffect::Copy);
        else if (token == u"execute"sv)
            result.append(AriaDropEffect::Execute);
        else if (token == u"link"sv)
            result.append(AriaDropEffect::Link);
        else if (token == u"move"sv)
            result.append(AriaDropEffect::Move);
        else if (token == u"popup"sv)
            result.append(AriaDropEffect::Popup);
        return IterationDecision::Continue;
    });

    // None combined with any other token value is ignored
    if (result.is_empty())
        result.append(AriaDropEffect::None);

    return result;
}

AriaHasPopup AriaData::parse_aria_has_popup(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"false"sv))
        return AriaHasPopup::False;
    if (value_equals(value, u"true"sv))
        return AriaHasPopup::True;
    if (value_equals(value, u"menu"sv))
        return AriaHasPopup::Menu;
    if (value_equals(value, u"listbox"sv))
        return AriaHasPopup::Listbox;
    if (value_equals(value, u"tree"sv))
        return AriaHasPopup::Tree;
    if (value_equals(value, u"grid"sv))
        return AriaHasPopup::Grid;
    if (value_equals(value, u"dialog"sv))
        return AriaHasPopup::Dialog;
    return AriaHasPopup::False;
}

AriaInvalid AriaData::parse_aria_invalid(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"grammar"sv))
        return AriaInvalid::Grammar;
    if (value_equals(value, u"false"sv))
        return AriaInvalid::False;
    if (value_equals(value, u"spelling"sv))
        return AriaInvalid::Spelling;
    if (value_equals(value, u"true"sv))
        return AriaInvalid::True;
    return AriaInvalid::False;
}

Optional<AriaLive> AriaData::parse_aria_live(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"assertive"sv))
        return AriaLive::Assertive;
    if (value_equals(value, u"off"sv))
        return AriaLive::Off;
    if (value_equals(value, u"polite"sv))
        return AriaLive::Polite;
    return {};
}

Optional<AriaOrientation> AriaData::parse_aria_orientation(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"horizontal"sv))
        return AriaOrientation::Horizontal;
    if (value_equals(value, u"undefined"sv))
        return AriaOrientation::Undefined;
    if (value_equals(value, u"vertical"sv))
        return AriaOrientation::Vertical;
    return {};
}

Vector<AriaRelevant> AriaData::parse_aria_relevant(Optional<Utf16String> const& value)
{
    if (!value.has_value())
        return {};

    Vector<AriaRelevant> result;
    Vector<Utf16View> tokens;
    for_each_ascii_whitespace_separated_token(value->utf16_view(), [&](auto token) {
        tokens.append(token);
        return IterationDecision::Continue;
    });
    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == u"additions"sv) {
            if (i + 1 < tokens.size()) {
                if (tokens[i + 1] == u"text"sv) {
                    result.append(AriaRelevant::AdditionsText);
                    ++i;
                    continue;
                }
                if (tokens[i + 1] == u"removals"sv && i + 2 < tokens.size() && tokens[i + 2] == u"text"sv) {
                    result.append(AriaRelevant::All);
                    i += 2;
                    continue;
                }
            }
            result.append(AriaRelevant::Additions);
        } else if (tokens[i] == u"all"sv)
            result.append(AriaRelevant::All);
        else if (tokens[i] == u"removals"sv)
            result.append(AriaRelevant::Removals);
        else if (tokens[i] == u"text"sv)
            result.append(AriaRelevant::Text);
    }

    if (result.is_empty())
        result.append(AriaRelevant::AdditionsText);

    return result;
}

AriaSort AriaData::parse_aria_sort(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"ascending"sv))
        return AriaSort::Ascending;
    if (value_equals(value, u"descending"sv))
        return AriaSort::Descending;
    if (value_equals(value, u"none"sv))
        return AriaSort::None;
    if (value_equals(value, u"other"sv))
        return AriaSort::Other;
    return AriaSort::None;
}

Optional<bool> AriaData::parse_optional_true_false(Optional<Utf16String> const& value)
{
    if (value_equals(value, u"true"sv))
        return true;
    if (value_equals(value, u"false"sv))
        return false;
    return {};
}

}
