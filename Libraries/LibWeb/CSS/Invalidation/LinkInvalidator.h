/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Document;
class Element;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_hyperlink_state_change(DOM::Element&);
void invalidate_style_after_legacy_link_color_change(DOM::Document&);

}
