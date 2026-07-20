/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::Editing {

// Convert between a single legacy <font> run and the inline style form used while moving rendered editing content.
// Styled serialization and paragraph transfer share this compatibility policy, independently of paste cleanup.
void remove_single_run_legacy_font_style(DOM::Node&);
void materialize_single_run_legacy_font_style(DOM::Node&);
void materialize_single_run_legacy_font_style(DOM::Node&, DOM::Node& rendered_style_source);

}
