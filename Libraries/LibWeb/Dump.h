/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web {

WEB_API void dump_tree(HTML::TraversableNavigable&);
void dump_tree(StringBuilder&, DOM::Node const&);
WEB_API void dump_tree(DOM::Node const&);
WEB_API void dump_tree(StringBuilder&, Layout::Node const&, bool show_cascaded_properties = false, bool colorize = false);
WEB_API void dump_tree(Layout::Node const&, bool show_cascaded_properties = false);
WEB_API void dump_tree(StringBuilder&, Painting::Paintable const&, bool colorize = false, int indent = 0);
WEB_API void dump_tree(Painting::Paintable const&);
void dump_sheet(StringBuilder&, CSS::StyleSheet const&, int indent_levels = 0);
WEB_API void dump_sheet(CSS::StyleSheet const&);
void dump_rule(StringBuilder&, CSS::CSSRule const&, int indent_levels = 0);
void dump_rule(CSS::CSSRule const&);
void dump_style_properties(StringBuilder&, CSS::CSSStyleProperties const&, int indent_levels = 0);
void dump_descriptors(StringBuilder&, CSS::CSSDescriptors const&, int indent_levels = 0);
void dump_selector(StringBuilder&, CSS::Selector const&, int indent_levels = 0);
void dump_selector(CSS::Selector const&);

inline void dump_indent(StringBuilder& builder, int indent_levels)
{
    builder.append_repeated("  "sv, indent_levels);
}

}
