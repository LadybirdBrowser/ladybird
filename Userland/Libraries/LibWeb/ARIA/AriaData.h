/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/Forward.h>

namespace Web::ARIA {

// https://www.w3.org/TR/wai-aria-1.2/#valuetype_tristate
enum class Tristate {
    True,
    False,
    Mixed,
    Undefined
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-autocomplete
enum class AriaAutocomplete {
    // When a user is providing input, text suggesting one way to complete the provided input may be dynamically inserted after the caret.
    Inline,
    // When a user is providing input, an element containing a collection of values that could complete the provided input may be displayed.
    List,
    // When a user is providing input, an element containing a collection of values that could complete the provided input may be displayed.
    // If displayed, one value in the collection is automatically selected, and the text needed to complete the automatically selected value appears after the caret in the input
    Both,
    // When a user is providing input, an automatic suggestion that attempts to predict how the user intends to complete the input is not displayed.
    None
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-current
enum class AriaCurrent {
    // Represents the current page within a set of pages.
    Page,
    // Represents the current step within a process.
    Step,
    // Represents the current location within an environment or context.
    Location,
    // Represents the current date within a collection of dates.
    Date,
    // Represents the current time within a set of times.
    Time,
    // Represents the current item within a set.
    True,
    // Does not represent the current item within a set.
    False
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-dropeffect
enum class AriaDropEffect {
    // A duplicate of the source object will be dropped into the target.
    Copy,
    // A function supported by the drop target is executed, using the drag source as an input.
    Execute,
    // A reference or shortcut to the dragged object will be created in the target object.
    Link,
    // The source object will be removed from its current location and dropped into the target.
    Move,
    // No operation can be performed; effectively cancels the drag operation if an attempt is made to drop on this object.
    // Ignored if combined with any other token value. e.g., 'none copy' is equivalent to a 'copy' value.
    None,
    // There is a popup menu or dialog that allows the user to choose one of the drag operations (copy, move, link, execute) and any other drag functionality, such as cancel.
    Popup
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-haspopup
enum class AriaHasPopup {
    // Indicates the element does not have a popup.
    False,
    // Indicates the popup is a menu.
    True,
    // Indicates the popup is a menu.
    Menu,
    // Indicates the popup is a listbox.
    Listbox,
    // Indicates the popup is a tree.
    Tree,
    // Indicates the popup is a grid.
    Grid,
    // Indicates the popup is a dialog.
    Dialog
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-invalid
enum class AriaInvalid {
    // A grammatical error was detected.
    Grammar,
    // There are no detected errors in the value.
    False,
    // A spelling error was detected.
    Spelling,
    // The value entered by the user has failed validation.
    True
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-live
enum class AriaLive {
    // Indicates that updates to the region have the highest priority and should be presented the user immediately.
    Assertive,
    // Indicates that updates to the region should not be presented to the user unless the user is currently focused on that region.
    Off,
    // Indicates that updates to the region should be presented at the next graceful opportunity, such as at the end of speaking the current sentence or when the user pauses typing.
    Polite
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-orientation
enum class AriaOrientation {
    // The element is oriented horizontally.
    Horizontal,
    // The element's orientation is unknown/ambiguous.
    Undefined,
    // The element is oriented vertically.
    Vertical
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-relevant
enum class AriaRelevant {
    // Element nodes are added to the accessibility tree within the live region.
    Additions,
    // Equivalent to the combination of values, "additions text".
    AdditionsText,
    // Equivalent to the combination of all values, "additions removals text".
    All,
    // Text content, a text alternative, or an element node within the live region is removed from the accessibility tree.
    Removals,
    // Text content or a text alternative is added to any descendant in the accessibility tree of the live region.
    Text
};

// https://www.w3.org/TR/wai-aria-1.2/#aria-sort
enum class AriaSort {
    // Items are sorted in ascending order by this column.
    Ascending,
    // Items are sorted in descending order by this column.
    Descending,
    // There is no defined sort applied to the column.
    None,
    // A sort algorithm other than ascending or descending has been applied.
    Other
};

class AriaData {
public:
    AriaData() { }

    static ErrorOr<NonnullOwnPtr<AriaData>> build_data(ARIAMixin const& mixin) { return adopt_nonnull_own_or_enomem(new (nothrow) AriaData(mixin)); }

    Optional<String> aria_active_descendant_or_default() const;
    bool aria_atomic_or_default(bool default_value = false) const;
    AriaAutocomplete aria_auto_complete_or_default() const;
    String aria_braille_label_or_default() const;
    String aria_braille_role_description_or_default() const;
    bool aria_busy_or_default() const;
    Tristate aria_checked_or_default() const;
    Optional<i32> aria_col_count_or_default() const;
    Optional<i32> aria_col_index_or_default() const;
    String aria_col_index_text_or_default() const;
    Optional<i32> aria_col_span_or_default() const;
    Vector<String> aria_controls_or_default() const;
    AriaCurrent aria_current_or_default() const;
    Vector<String> aria_described_by_or_default() const;
    String aria_description_or_default() const;
    Optional<String> aria_details_or_default() const;
    bool aria_disabled_or_default() const;
    Vector<AriaDropEffect> aria_drop_effect_or_default() const;
    Optional<String> aria_error_message_or_default() const;
    Optional<bool> aria_expanded_or_default() const;
    Vector<String> aria_flow_to_or_default() const;
    Optional<bool> aria_grabbed_or_default() const;
    AriaHasPopup aria_has_popup_or_default() const;
    Optional<bool> aria_hidden_or_default() const;
    AriaInvalid aria_invalid_or_default() const;
    String aria_key_shortcuts_or_default() const;
    String aria_label_or_default() const;
    Vector<String> aria_labelled_by_or_default() const;
    Optional<i32> aria_level_or_default() const;
    AriaLive aria_live_or_default(AriaLive default_value = AriaLive::Off) const;
    bool aria_modal_or_default() const;
    bool aria_multi_line_or_default() const;
    bool aria_multi_selectable_or_default() const;
    AriaOrientation aria_orientation_or_default(AriaOrientation default_value = AriaOrientation::Undefined) const;
    Vector<String> aria_owns_or_default() const;
    String aria_placeholder_or_default() const;
    Optional<i32> aria_pos_in_set_or_default() const;
    Tristate aria_pressed_or_default() const;
    bool aria_read_only_or_default() const;
    Vector<AriaRelevant> aria_relevant_or_default() const;
    bool aria_required_or_default() const;
    String aria_role_description_or_default() const;
    Optional<i32> aria_row_count_or_default() const;
    Optional<i32> aria_row_index_or_default() const;
    String aria_row_index_text_or_default() const;
    Optional<i32> aria_row_span_or_default() const;
    Optional<bool> aria_selected_or_default() const;
    Optional<i32> aria_set_size_or_default() const;
    AriaSort aria_sort_or_default() const;
    Optional<f64> aria_value_max_or_default(Optional<f64> default_value = {}) const;
    Optional<f64> aria_value_min_or_default(Optional<f64> default_value = {}) const;
    Optional<f64> aria_value_now_or_default() const;
    String aria_value_text_or_default() const;

private:
    explicit AriaData(ARIAMixin const&);

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_true-false
    // The default value for this value type is false unless otherwise specified.
    static bool parse_true_false(Optional<String> const&);

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_tristate
    // The default value for this value type is undefined unless otherwise specified.
    static Tristate parse_tristate(Optional<String> const&);

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_true-false-undefined
    // The default value for this value type is undefined unless otherwise specified.
    static Optional<bool> parse_true_false_undefined(Optional<String> const&);

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_integer
    static Optional<i32> parse_integer(Optional<String> const&);

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_number
    static Optional<f64> parse_number(Optional<String> const&);

    static AriaAutocomplete parse_aria_autocomplete(Optional<String> const&);
    static AriaCurrent parse_aria_current(Optional<String> const&);
    static Vector<AriaDropEffect> parse_aria_drop_effect(Optional<String> const&);
    static AriaHasPopup parse_aria_has_popup(Optional<String> const&);
    static AriaInvalid parse_aria_invalid(Optional<String> const&);
    static Optional<AriaLive> parse_aria_live(Optional<String> const&);
    static Optional<AriaOrientation> parse_aria_orientation(Optional<String> const&);
    static Vector<AriaRelevant> parse_aria_relevant(Optional<String> const&);
    static AriaSort parse_aria_sort(Optional<String> const&);
    static Optional<bool> parse_optional_true_false(Optional<String> const&);

    Optional<String> m_aria_active_descendant;
    Optional<bool> m_aria_atomic;
    AriaAutocomplete m_aria_auto_complete;
    String m_aria_braille_label;
    String m_aria_braille_role_description;
    bool m_aria_busy;
    Tristate m_aria_checked;
    Optional<i32> m_aria_col_count;
    Optional<i32> m_aria_col_index;
    String m_aria_col_index_text;
    Optional<i32> m_aria_col_span;
    Vector<String> m_aria_controls;
    AriaCurrent m_aria_current;
    Vector<String> m_aria_described_by;
    String m_aria_description;
    Optional<String> m_aria_details;
    bool m_aria_disabled;
    Vector<AriaDropEffect> m_aria_drop_effect;
    Optional<String> m_aria_error_message;
    Optional<bool> m_aria_expanded;
    Vector<String> m_aria_flow_to;
    Optional<bool> m_aria_grabbed;
    AriaHasPopup m_aria_has_popup;
    Optional<bool> m_aria_hidden;
    AriaInvalid m_aria_invalid;
    String m_aria_key_shortcuts;
    String m_aria_label;
    Vector<String> m_aria_labelled_by;
    Optional<i32> m_aria_level;
    Optional<AriaLive> m_aria_live;
    bool m_aria_modal;
    bool m_aria_multi_line;
    bool m_aria_multi_selectable;
    Optional<AriaOrientation> m_aria_orientation;
    Vector<String> m_aria_owns;
    String m_aria_placeholder;
    Optional<i32> m_aria_pos_in_set;
    Tristate m_aria_pressed;
    bool m_aria_read_only;
    Vector<AriaRelevant> m_aria_relevant;
    bool m_aria_required;
    String m_aria_role_description;
    Optional<i32> m_aria_row_count;
    Optional<i32> m_aria_row_index;
    String m_aria_row_index_text;
    Optional<i32> m_aria_row_span;
    Optional<bool> m_aria_selected;
    Optional<i32> m_aria_set_size;
    AriaSort m_aria_sort;
    Optional<f64> m_aria_value_max;
    Optional<f64> m_aria_value_min;
    Optional<f64> m_aria_value_now;
    String m_aria_value_text;
};

}
