/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::CSS {

// Drop the rule cache of the scope a sheet was added to or removed from, either via a <style> element or via
// adoptedStyleSheets. The caller is responsible for updating the sheet's owning_documents_or_shadow_roots and any
// list-membership before invoking this.
void invalidate_rule_cache_after_style_sheet_change(DOM::Node& document_or_shadow_root, CSSStyleSheet const& sheet);

// Drop the rule cache of every scope that owns `style_sheet`, because the rules it contributes to them moved.
void invalidate_rule_cache_for_style_sheet_owners(CSSStyleSheet const& style_sheet);

}
