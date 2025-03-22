/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/Forward.h>

namespace Web {

void dump_tree(HTML::TraversableNavigable&);
void dump_tree(StringBuilder&, DOM::Node const&);
void dump_tree(DOM::Node const&);
void dump_tree(StringBuilder&, Layout::Node const&, bool show_box_model = false, bool show_cascaded_properties = false, bool colorize = false);
void dump_tree(Layout::Node const&, bool show_box_model = true, bool show_cascaded_properties = false);
void dump_tree(StringBuilder&, Painting::Paintable const&, bool colorize = false, int indent = 0);
void dump_tree(Painting::Paintable const&);
void dump_sheet(StringBuilder&, CSS::StyleSheet const&);
void dump_sheet(CSS::StyleSheet const&);
void dump_rule(StringBuilder&, CSS::CSSRule const&, int indent_levels = 0);
void dump_rule(CSS::CSSRule const&);
void dump_style_properties(StringBuilder&, CSS::CSSStyleProperties const&, int indent_levels = 0);
void dump_font_face_rule(StringBuilder&, CSS::CSSFontFaceRule const&, int indent_levels = 0);
void dump_import_rule(StringBuilder&, CSS::CSSImportRule const&, int indent_levels = 0);
void dump_media_rule(StringBuilder&, CSS::CSSMediaRule const&, int indent_levels = 0);
void dump_style_rule(StringBuilder&, CSS::CSSStyleRule const&, int indent_levels = 0);
void dump_supports_rule(StringBuilder&, CSS::CSSSupportsRule const&, int indent_levels = 0);
void dump_property_rule(StringBuilder&, CSS::CSSPropertyRule const&, int indent_levels = 0);
void dump_namespace_rule(StringBuilder&, CSS::CSSNamespaceRule const&, int indent_levels = 0);
void dump_nested_declarations(StringBuilder&, CSS::CSSNestedDeclarations const&, int indent_levels = 0);
void dump_layer_block_rule(StringBuilder&, CSS::CSSLayerBlockRule const&, int indent_levels = 0);
void dump_layer_statement_rule(StringBuilder&, CSS::CSSLayerStatementRule const&, int indent_levels = 0);
void dump_selector(StringBuilder&, CSS::Selector const&, int indent_levels = 0);
void dump_selector(CSS::Selector const&);

}
