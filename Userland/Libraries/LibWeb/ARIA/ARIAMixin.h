/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::ARIA {

class ARIAMixin {

public:
    virtual ~ARIAMixin() = default;

    virtual Optional<String> role() const = 0;
    virtual WebIDL::ExceptionOr<void> set_role(Optional<String> const&) = 0;

    virtual Optional<String> aria_active_descendant() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_active_descendant(Optional<String> const&) = 0;

    virtual Optional<String> aria_atomic() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_atomic(Optional<String> const&) = 0;

    virtual Optional<String> aria_auto_complete() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_auto_complete(Optional<String> const&) = 0;

    virtual Optional<String> aria_braille_label() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_braille_label(Optional<String> const&) = 0;

    virtual Optional<String> aria_braille_role_description() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_braille_role_description(Optional<String> const&) = 0;

    virtual Optional<String> aria_busy() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_busy(Optional<String> const&) = 0;

    virtual Optional<String> aria_checked() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_checked(Optional<String> const&) = 0;

    virtual Optional<String> aria_col_count() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_col_count(Optional<String> const&) = 0;

    virtual Optional<String> aria_col_index() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_col_index(Optional<String> const&) = 0;

    virtual Optional<String> aria_col_index_text() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_col_index_text(Optional<String> const&) = 0;

    virtual Optional<String> aria_col_span() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_col_span(Optional<String> const&) = 0;

    virtual Optional<String> aria_controls() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_controls(Optional<String> const&) = 0;

    virtual Optional<String> aria_current() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_current(Optional<String> const&) = 0;

    virtual Optional<String> aria_described_by() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_described_by(Optional<String> const&) = 0;

    virtual Optional<String> aria_description() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_description(Optional<String> const&) = 0;

    virtual Optional<String> aria_details() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_details(Optional<String> const&) = 0;

    virtual Optional<String> aria_disabled() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_disabled(Optional<String> const&) = 0;

    virtual Optional<String> aria_drop_effect() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_drop_effect(Optional<String> const&) = 0;

    virtual Optional<String> aria_error_message() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_error_message(Optional<String> const&) = 0;

    virtual Optional<String> aria_expanded() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_expanded(Optional<String> const&) = 0;

    virtual Optional<String> aria_flow_to() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_flow_to(Optional<String> const&) = 0;

    virtual Optional<String> aria_grabbed() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_grabbed(Optional<String> const&) = 0;

    virtual Optional<String> aria_has_popup() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_has_popup(Optional<String> const&) = 0;

    virtual Optional<String> aria_hidden() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_hidden(Optional<String> const&) = 0;

    virtual Optional<String> aria_invalid() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_invalid(Optional<String> const&) = 0;

    virtual Optional<String> aria_key_shortcuts() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_key_shortcuts(Optional<String> const&) = 0;

    virtual Optional<String> aria_label() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_label(Optional<String> const&) = 0;

    virtual Optional<String> aria_labelled_by() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_labelled_by(Optional<String> const&) = 0;

    virtual Optional<String> aria_level() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_level(Optional<String> const&) = 0;

    virtual Optional<String> aria_live() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_live(Optional<String> const&) = 0;

    virtual Optional<String> aria_modal() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_modal(Optional<String> const&) = 0;

    virtual Optional<String> aria_multi_line() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_multi_line(Optional<String> const&) = 0;

    virtual Optional<String> aria_multi_selectable() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_multi_selectable(Optional<String> const&) = 0;

    virtual Optional<String> aria_orientation() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_orientation(Optional<String> const&) = 0;

    virtual Optional<String> aria_owns() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_owns(Optional<String> const&) = 0;

    virtual Optional<String> aria_placeholder() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_placeholder(Optional<String> const&) = 0;

    virtual Optional<String> aria_pos_in_set() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_pos_in_set(Optional<String> const&) = 0;

    virtual Optional<String> aria_pressed() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_pressed(Optional<String> const&) = 0;

    virtual Optional<String> aria_read_only() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_read_only(Optional<String> const&) = 0;

    virtual Optional<String> aria_relevant() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_relevant(Optional<String> const&) = 0;

    virtual Optional<String> aria_required() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_required(Optional<String> const&) = 0;

    virtual Optional<String> aria_role_description() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_role_description(Optional<String> const&) = 0;

    virtual Optional<String> aria_row_count() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_row_count(Optional<String> const&) = 0;

    virtual Optional<String> aria_row_index() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_row_index(Optional<String> const&) = 0;

    virtual Optional<String> aria_row_index_text() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_row_index_text(Optional<String> const&) = 0;

    virtual Optional<String> aria_row_span() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_row_span(Optional<String> const&) = 0;

    virtual Optional<String> aria_selected() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_selected(Optional<String> const&) = 0;

    virtual Optional<String> aria_set_size() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_set_size(Optional<String> const&) = 0;

    virtual Optional<String> aria_sort() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_sort(Optional<String> const&) = 0;

    virtual Optional<String> aria_value_max() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_value_max(Optional<String> const&) = 0;

    virtual Optional<String> aria_value_min() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_value_min(Optional<String> const&) = 0;

    virtual Optional<String> aria_value_now() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_value_now(Optional<String> const&) = 0;

    virtual Optional<String> aria_value_text() const = 0;
    virtual WebIDL::ExceptionOr<void> set_aria_value_text(Optional<String> const&) = 0;

    // https://www.w3.org/TR/html-aria/#docconformance
    virtual Optional<Role> default_role() const { return {}; }

    Optional<Role> role_or_default() const;

    // https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion
    virtual bool exclude_from_accessibility_tree() const = 0;

    // https://www.w3.org/TR/wai-aria-1.2/#tree_inclusion
    virtual bool include_in_accessibility_tree() const = 0;

    bool has_global_aria_attribute() const;

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_idref
    Optional<String> parse_id_reference(Optional<String> const&) const;

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_idref_list
    Vector<String> parse_id_reference_list(Optional<String> const&) const;

protected:
    ARIAMixin() = default;

    virtual bool id_reference_exists(String const&) const = 0;
};

}
