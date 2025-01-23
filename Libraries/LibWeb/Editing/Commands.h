/*
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::Editing {

// https://w3c.github.io/editing/docs/execCommand/#properties-of-commands
struct CommandDefinition {
    FlyString const& command;
    Function<bool(DOM::Document&, String const&)> action {};
    Function<bool(DOM::Document const&)> indeterminate {};
    Function<bool(DOM::Document const&)> state {};
    Function<String(DOM::Document const&)> value {};
    Optional<CSS::PropertyID> relevant_css_property {};

    // https://w3c.github.io/editing/docs/execCommand/#preserves-overrides
    bool preserves_overrides { false };

    // https://w3c.github.io/editing/docs/execCommand/#inline-command-activated-values
    Vector<StringView> inline_activated_values {};

    // https://w3c.github.io/editing/docs/execCommand/#dfn-map-an-edit-command-to-input-type-value
    FlyString mapped_value {};
};

Optional<CommandDefinition const&> find_command_definition(FlyString const&);

// Command implementations
bool command_back_color_action(DOM::Document&, String const&);
bool command_bold_action(DOM::Document&, String const&);
bool command_create_link_action(DOM::Document&, String const&);
bool command_default_paragraph_separator_action(DOM::Document&, String const&);
String command_default_paragraph_separator_value(DOM::Document const&);
bool command_delete_action(DOM::Document&, String const&);
bool command_font_name_action(DOM::Document&, String const&);
bool command_font_size_action(DOM::Document&, String const&);
String command_font_size_value(DOM::Document const&);
bool command_fore_color_action(DOM::Document&, String const&);
bool command_format_block_action(DOM::Document&, String const&);
bool command_format_block_indeterminate(DOM::Document const&);
String command_format_block_value(DOM::Document const&);
bool command_forward_delete_action(DOM::Document&, String const&);
bool command_indent_action(DOM::Document&, String const&);
bool command_insert_horizontal_rule_action(DOM::Document&, String const&);
bool command_insert_html_action(DOM::Document&, String const&);
bool command_insert_image_action(DOM::Document&, String const&);
bool command_insert_linebreak_action(DOM::Document&, String const&);
bool command_insert_ordered_list_action(DOM::Document&, String const&);
bool command_insert_ordered_list_indeterminate(DOM::Document const&);
bool command_insert_ordered_list_state(DOM::Document const&);
bool command_insert_paragraph_action(DOM::Document&, String const&);
bool command_insert_text_action(DOM::Document&, String const&);
bool command_insert_unordered_list_action(DOM::Document&, String const&);
bool command_insert_unordered_list_indeterminate(DOM::Document const&);
bool command_insert_unordered_list_state(DOM::Document const&);
bool command_italic_action(DOM::Document&, String const&);
bool command_justify_center_action(DOM::Document&, String const&);
bool command_justify_center_indeterminate(DOM::Document const&);
bool command_justify_center_state(DOM::Document const&);
String command_justify_center_value(DOM::Document const&);
bool command_justify_full_action(DOM::Document&, String const&);
bool command_justify_full_indeterminate(DOM::Document const&);
bool command_justify_full_state(DOM::Document const&);
String command_justify_full_value(DOM::Document const&);
bool command_justify_left_action(DOM::Document&, String const&);
bool command_justify_left_indeterminate(DOM::Document const&);
bool command_justify_left_state(DOM::Document const&);
String command_justify_left_value(DOM::Document const&);
bool command_justify_right_action(DOM::Document&, String const&);
bool command_justify_right_indeterminate(DOM::Document const&);
bool command_justify_right_state(DOM::Document const&);
String command_justify_right_value(DOM::Document const&);
bool command_outdent_action(DOM::Document&, String const&);
bool command_remove_format_action(DOM::Document&, String const&);
bool command_select_all_action(DOM::Document&, String const&);
bool command_strikethrough_action(DOM::Document&, String const&);
bool command_style_with_css_action(DOM::Document&, String const&);
bool command_style_with_css_state(DOM::Document const&);
bool command_subscript_action(DOM::Document&, String const&);
bool command_subscript_indeterminate(DOM::Document const&);
bool command_superscript_action(DOM::Document&, String const&);
bool command_superscript_indeterminate(DOM::Document const&);
bool command_underline_action(DOM::Document&, String const&);
bool command_unlink_action(DOM::Document&, String const&);
bool command_use_css_action(DOM::Document&, String const&);

}
