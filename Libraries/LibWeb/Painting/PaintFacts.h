/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

WEB_API void push_paint_facts_after_style_attach(Layout::NodeWithStyle&);
WEB_API void push_form_control_paint_facts(HTML::HTMLInputElement&);

}
