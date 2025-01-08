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
bool command_forward_delete_action(DOM::Document&, String const&);
bool command_insert_linebreak_action(DOM::Document&, String const&);
bool command_insert_paragraph_action(DOM::Document&, String const&);
bool command_italic_action(DOM::Document&, String const&);
bool command_remove_format_action(DOM::Document&, String const&);
bool command_style_with_css_action(DOM::Document&, String const&);
bool command_style_with_css_state(DOM::Document const&);

}
