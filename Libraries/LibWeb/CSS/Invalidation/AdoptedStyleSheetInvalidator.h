/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Node;

}

namespace Web::CSS {

class StyleSheetState;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_adopting_style_sheet(DOM::Node& document_or_shadow_root, StyleSheetState&);
void invalidate_style_after_removing_adopted_style_sheet(DOM::Node& document_or_shadow_root, StyleSheetState&);

}
