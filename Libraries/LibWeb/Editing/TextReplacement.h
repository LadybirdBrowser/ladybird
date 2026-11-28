/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// Once the user types a character that ends a word, asks the UI process for the replacement the user set up for that
// word (a macOS Text Replacement, e.g.), and replaces the word with it, as an edit that fires beforeinput and input
// events with the "insertReplacementText" input type, and that undo takes back. Nothing happens if the focus has moved
// away from what the user typed into.
void replace_word_after_typing(DOM::Document&, InputEventsTarget const& typed_into, u32 typed_code_point);

}
