/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibGfx/Font/Font.h>
#include <LibWeb/ARIA/AttributeNames.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/DOM/AccessibilityTreeNode.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMeterElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWebView/AccessibilityNodeData.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(AccessibilityTreeNode);

GC::Ref<AccessibilityTreeNode> AccessibilityTreeNode::create(Document* document, DOM::Node const* value)
{
    return document->realm().create<AccessibilityTreeNode>(value);
}

AccessibilityTreeNode::AccessibilityTreeNode(GC::Ptr<DOM::Node const> value)
    : m_value(value)
{
    m_children = {};
}

void AccessibilityTreeNode::serialize_tree_as_json(JsonObjectSerializer<StringBuilder>& object, Document const& document) const
{
    if (value()->is_document()) {
        VERIFY_NOT_REACHED();
    } else if (value()->is_element()) {
        auto const* element = static_cast<DOM::Element const*>(value().ptr());

        if (element->include_in_accessibility_tree()) {
            MUST(object.add("type"sv, "element"sv));

            auto role = element->role_or_default();
            bool has_role = role.has_value() && !ARIA::is_abstract_role(*role);

            auto name = MUST(element->accessible_name(document));
            MUST(object.add("name"sv, name));
            auto description = MUST(element->accessible_description(document));
            MUST(object.add("description"sv, description));

            if (has_role)
                MUST(object.add("role"sv, ARIA::role_name(*role)));
            else
                MUST(object.add("role"sv, ""sv));
        } else {
            VERIFY_NOT_REACHED();
        }
    } else if (value()->is_text()) {
        MUST(object.add("type"sv, "text"sv));

        auto const* text_node = static_cast<DOM::Text const*>(value().ptr());
        MUST(object.add("name"sv, text_node->data().to_utf8()));
        MUST(object.add("role"sv, "text leaf"sv));
    }

    MUST(object.add("id"sv, value()->unique_id().value()));

    if (value()->has_child_nodes()) {
        auto node_children = MUST(object.add_array("children"sv));
        for (auto& child : children()) {
            if (child->value()->is_uninteresting_whitespace_node())
                continue;
            JsonObjectSerializer<StringBuilder> child_object = MUST(node_children.add_object());
            child->serialize_tree_as_json(child_object, document);
            MUST(child_object.finish());
        }
        MUST(node_children.finish());
    }
}

void AccessibilityTreeNode::serialize_tree_as_node_data(Vector<WebView::AccessibilityNodeData>& out, Document const& document, i64 parent_id) const
{
    WebView::AccessibilityNodeData node_data;
    node_data.parent_id = parent_id;

    if (value()->is_element()) {
        auto const& element = static_cast<DOM::Element const&>(*value());

        node_data.id = static_cast<i64>(element.unique_id().value());
        auto role = element.role_or_default();
        if (role.has_value() && !ARIA::is_abstract_role(*role)) {
            // If the role is none/presentation but the element was included due to conflict resolution (has global ARIA
            // attributes), then use the element's implicit semantic role.
            if ((*role == ARIA::Role::none || *role == ARIA::Role::presentation)
                && element.has_global_aria_attribute()) {
                if (is<HTML::HTMLImageElement>(element))
                    node_data.role = "image"_string;
                else
                    node_data.role = MUST(String::from_utf8(ARIA::role_name(*role)));
            } else {
                node_data.role = MUST(String::from_utf8(ARIA::role_name(*role)));
            }
        }

        // option elements usually have no layout node (they only render inside a popup) — so accessible_name returns
        // empty; thus for those, fall back to the option element's label/text.
        if (auto const* option = as_if<HTML::HTMLOptionElement>(element)) {
            auto label = option->label();
            if (!label.is_empty())
                node_data.name = label;
            else
                node_data.name = option->text().to_utf8();
        } else {
            node_data.name = MUST(element.accessible_name(document));
        }
        if (auto trimmed = node_data.name.bytes_as_string_view().trim_whitespace(); trimmed != node_data.name.bytes_as_string_view())
            node_data.name = MUST(String::from_utf8(trimmed));
        node_data.description = MUST(element.accessible_description(document));
        if (node_data.description.is_empty()) {
            if (auto desc = element.get_attribute(ARIA::AttributeNames::aria_description); desc.has_value())
                node_data.description = desc.release_value();
        }
        node_data.bounds = element.get_bounding_client_rect().to_type<int>();

        if (auto level = element.aria_level(); level.has_value()) {
            if (auto parsed = level->bytes_as_string_view().to_number<i32>(); parsed.has_value())
                node_data.heading_level = *parsed;
        }

        if (auto live_attr = element.get_attribute(ARIA::AttributeNames::aria_live); live_attr.has_value()) {
            node_data.live = live_attr.release_value();
        } else if (role.has_value() && ARIA::is_live_region_role(*role)) {
            if (*role == ARIA::Role::alert)
                node_data.live = "assertive"_string;
            else
                node_data.live = "polite"_string;
        }

        if (element.is_actually_disabled())
            node_data.is_disabled = true;

        if (auto const* cell = as_if<HTML::HTMLTableCellElement>(element)) {
            node_data.column_span = static_cast<i32>(cell->col_span());
            node_data.row_span = static_cast<i32>(cell->row_span());

            // Determine cell's row index by walking up to the table and iterating rows, and its column index by
            // counting preceding cells in the same row (accounting for colspan).
            auto row_result = cell->closest({ HTML::TagNames::tr });
            if (!row_result.is_exception() && row_result.value()) {
                auto const* row = row_result.value();
                auto table_result = row->closest({ HTML::TagNames::table });
                if (!table_result.is_exception() && table_result.value()) {
                    auto const& table = static_cast<HTML::HTMLTableElement const&>(*table_result.value());
                    auto rows = table.rows();
                    for (u32 i = 0; i < rows->length(); ++i) {
                        if (rows->item(i) == row) {
                            node_data.cell_row_index = static_cast<i32>(i);
                            break;
                        }
                    }
                }
                // Column index: sum of col_span of previous siblings.
                i32 col_index = 0;
                for (auto const* prev = row->first_child(); prev && prev != cell; prev = prev->next_sibling()) {
                    if (auto const* prev_cell = as_if<HTML::HTMLTableCellElement>(prev))
                        col_index += static_cast<i32>(prev_cell->col_span());
                }
                node_data.cell_column_index = col_index;
            }
        }

        if (auto const* table = as_if<HTML::HTMLTableElement>(element)) {
            auto rows = table->rows();
            node_data.table_row_count = static_cast<i32>(rows->length());
            // Column count: maximum over all rows of summed col_span.
            i32 max_cols = 0;
            for (u32 i = 0; i < rows->length(); ++i) {
                auto* row = rows->item(i);
                if (!row)
                    continue;
                i32 cols = 0;
                for (auto const* c = row->first_child(); c; c = c->next_sibling()) {
                    if (auto const* cell = as_if<HTML::HTMLTableCellElement>(c))
                        cols += static_cast<i32>(cell->col_span());
                }
                max_cols = max(max_cols, cols);
            }
            node_data.table_column_count = max_cols;

            for (auto const* c = table->first_child(); c; c = c->next_sibling()) {
                if (c->is_element()
                    && static_cast<DOM::Element const&>(*c).tag_name().equals_ignoring_ascii_case("caption"sv)) {
                    node_data.table_caption_id = static_cast<i64>(c->unique_id().value());
                    break;
                }
            }

            if (auto summary = table->get_attribute(HTML::AttributeNames::summary); summary.has_value())
                node_data.table_summary = summary.release_value();

            // Column headers: first row's th cells (or any row with explicit scope=col). Row headers: first th cell in
            // each row (or any cell with scope=row).
            node_data.column_header_ids.resize(max_cols, true);
            for (i32 c = 0; c < max_cols; ++c)
                node_data.column_header_ids[c] = -1;
            node_data.row_header_ids.resize(rows->length(), true);
            for (u32 r = 0; r < rows->length(); ++r)
                node_data.row_header_ids[r] = -1;

            for (u32 row_index = 0; row_index < rows->length(); ++row_index) {
                auto* row = rows->item(row_index);
                if (!row)
                    continue;
                i32 col_index = 0;
                for (auto const* c = row->first_child(); c; c = c->next_sibling()) {
                    auto const* cell = as_if<HTML::HTMLTableCellElement>(c);
                    if (!cell)
                        continue;
                    auto cell_id = static_cast<i64>(cell->unique_id().value());
                    auto tag = cell->tag_name();
                    auto scope_attr = cell->get_attribute(HTML::AttributeNames::scope).value_or(String {});
                    auto scope = scope_attr.bytes_as_string_view();
                    bool is_th = tag.equals_ignoring_ascii_case("th"sv);
                    if (is_th && (scope == "col"sv || (scope.is_empty() && row_index == 0))) {
                        if (col_index < max_cols)
                            node_data.column_header_ids[col_index] = cell_id;
                    }
                    if (is_th && (scope == "row"sv || (scope.is_empty() && col_index == 0))) {
                        if (row_index < node_data.row_header_ids.size())
                            node_data.row_header_ids[row_index] = cell_id;
                    }
                    col_index += static_cast<i32>(cell->col_span());
                }
            }
        }

        // AtkValue fields for numeric-value elements.
        if (auto const* progress = as_if<HTML::HTMLProgressElement>(element)) {
            node_data.value_numeric = progress->value();
            node_data.value_minimum = 0.0;
            node_data.value_maximum = progress->max();
        } else if (auto const* meter = as_if<HTML::HTMLMeterElement>(element)) {
            node_data.value_numeric = meter->value();
            node_data.value_minimum = meter->min();
            node_data.value_maximum = meter->max();
        } else if (auto const* input = as_if<HTML::HTMLInputElement>(element)) {
            if (input->value_as_number_applies()) {
                node_data.value_numeric = input->value_as_number();
                if (auto min_attr = input->get_attribute(HTML::AttributeNames::min); min_attr.has_value()) {
                    if (auto parsed = min_attr->bytes_as_string_view().to_number<double>(); parsed.has_value())
                        node_data.value_minimum = *parsed;
                }
                if (auto max_attr = input->get_attribute(HTML::AttributeNames::max); max_attr.has_value()) {
                    if (auto parsed = max_attr->bytes_as_string_view().to_number<double>(); parsed.has_value())
                        node_data.value_maximum = *parsed;
                }
                if (auto step_attr = input->get_attribute(HTML::AttributeNames::step); step_attr.has_value()) {
                    if (auto parsed = step_attr->bytes_as_string_view().to_number<double>(); parsed.has_value())
                        node_data.value_step = *parsed;
                }
            }
        }

        // AtkSelection state.
        if (auto const* option = as_if<HTML::HTMLOptionElement>(element))
            node_data.is_selected = option->selected();

        if (auto const* input = as_if<HTML::HTMLInputElement>(element)) {
            if (input->type_state() == HTML::HTMLInputElement::TypeAttributeState::Checkbox
                || input->type_state() == HTML::HTMLInputElement::TypeAttributeState::RadioButton) {
                if (input->indeterminate())
                    node_data.checked_state = WebView::AccessibilityNodeData::CheckedState::Mixed;
                else
                    node_data.checked_state = input->checked()
                        ? WebView::AccessibilityNodeData::CheckedState::Checked
                        : WebView::AccessibilityNodeData::CheckedState::Unchecked;
            }
        } else if (auto aria_checked = element.get_attribute(ARIA::AttributeNames::aria_checked); aria_checked.has_value()) {
            auto value = aria_checked.value().bytes_as_string_view();
            if (value == "true"sv)
                node_data.checked_state = WebView::AccessibilityNodeData::CheckedState::Checked;
            else if (value == "false"sv)
                node_data.checked_state = WebView::AccessibilityNodeData::CheckedState::Unchecked;
            else if (value == "mixed"sv)
                node_data.checked_state = WebView::AccessibilityNodeData::CheckedState::Mixed;
        }

        if (auto const* details = as_if<HTML::HTMLDetailsElement>(element)) {
            node_data.expanded_state = details->has_attribute(HTML::AttributeNames::open)
                ? WebView::AccessibilityNodeData::ExpandedState::Expanded
                : WebView::AccessibilityNodeData::ExpandedState::Collapsed;
        } else if (auto aria_expanded = element.get_attribute(ARIA::AttributeNames::aria_expanded);
            aria_expanded.has_value()) {
            auto value = aria_expanded.value().bytes_as_string_view();
            if (value == "true"sv)
                node_data.expanded_state = WebView::AccessibilityNodeData::ExpandedState::Expanded;
            else if (value == "false"sv)
                node_data.expanded_state = WebView::AccessibilityNodeData::ExpandedState::Collapsed;
        }

        // Editable/multi-line/read-only for text inputs and textareas and contenteditable.
        auto is_readonly_attr = element.has_attribute(HTML::AttributeNames::readonly);
        auto is_required_attr = element.has_attribute(HTML::AttributeNames::required);
        if (auto const* input = as_if<HTML::HTMLInputElement>(element)) {
            // Only text-like inputs are "editable"; checkbox/radio/button/submit, etc., are form controls but not
            // text-editable. AT-SPI2's EDITABLE state refers specifically to text editing.
            using TAS = HTML::HTMLInputElement::TypeAttributeState;
            auto type = input->type_state();
            bool is_text_type = type == TAS::Text || type == TAS::Search || type == TAS::Email
                || type == TAS::Password || type == TAS::Telephone || type == TAS::URL || type == TAS::Number;
            if (is_text_type && !is_readonly_attr)
                node_data.is_editable = true;
            if (is_text_type && is_readonly_attr)
                node_data.is_read_only = true;
            if (is_required_attr)
                node_data.is_required = true;
        } else if (is<HTML::HTMLTextAreaElement>(element)) {
            if (!is_readonly_attr)
                node_data.is_editable = true;
            if (is_readonly_attr)
                node_data.is_read_only = true;
            if (is_required_attr)
                node_data.is_required = true;
            node_data.is_multi_line = true;
        } else if (element.is_editable()) {
            node_data.is_editable = true;
            node_data.is_multi_line = true; // contenteditable elements default to multi-line.
        }

        // aria-* overrides.
        if (auto ro = element.get_attribute(ARIA::AttributeNames::aria_read_only);
            ro.has_value() && ro.value().bytes_as_string_view() == "true"sv)
            node_data.is_read_only = true;
        if (auto req = element.get_attribute(ARIA::AttributeNames::aria_required);
            req.has_value() && req.value().bytes_as_string_view() == "true"sv)
            node_data.is_required = true;
        if (auto ml = element.get_attribute(ARIA::AttributeNames::aria_multi_line);
            ml.has_value() && ml.value().bytes_as_string_view() == "true"sv)
            node_data.is_multi_line = true;

        if (auto invalid = element.get_attribute(ARIA::AttributeNames::aria_invalid);
            invalid.has_value() && invalid.value().bytes_as_string_view() != "false"sv
            && !invalid.value().is_empty())
            node_data.is_invalid = true;

        if (auto const* select = as_if<HTML::HTMLSelectElement>(element)) {
            if (select->has_attribute(HTML::AttributeNames::multiple))
                node_data.is_multi_selectable = true;
        }
        if (auto ms = element.get_attribute(ARIA::AttributeNames::aria_multi_selectable);
            ms.has_value() && ms.value().bytes_as_string_view() == "true"sv)
            node_data.is_multi_selectable = true;

        if (auto pressed = element.get_attribute(ARIA::AttributeNames::aria_pressed);
            pressed.has_value() && pressed.value().bytes_as_string_view() == "true"sv)
            node_data.is_pressed = true;

        // Visited links.
        if (auto const* anchor = as_if<HTML::HTMLAnchorElement>(element)) {
            (void)anchor;
        }

        if (auto accesskey = element.get_attribute(HTML::AttributeNames::accesskey); accesskey.has_value())
            node_data.keybinding = accesskey.release_value();

        if (auto const* anchor = as_if<HTML::HTMLAnchorElement>(element))
            node_data.url = anchor->href();
        else if (auto const* area = as_if<HTML::HTMLAreaElement>(element))
            node_data.url = area->href();

        if (auto const* html_element = as_if<HTML::HTMLHtmlElement>(element)) {
            if (auto lang = html_element->get_attribute(HTML::AttributeNames::lang); lang.has_value())
                node_data.language = lang.release_value();
        }

        if (document.active_element() == &element)
            node_data.is_focused = true;
    } else if (value()->is_text()) {
        auto const& text_node = static_cast<DOM::Text const&>(*value());
        node_data.id = static_cast<i64>(text_node.unique_id().value());
        node_data.role = "text leaf"_string;
        node_data.name = text_node.data().to_utf8();

        // Text nodes have no bounding rect of their own — so, as an approximation, use the parent element's bounds.
        if (auto parent = text_node.parent_element()) {
            node_data.bounds = parent->get_bounding_client_rect().to_type<int>();

            // Read text formatting from the parent element's computed style. Text leaves inherit all of their
            // formatting from their containing element — so this covers the common case of mixed inline text and
            // formatting changes (e.g. <p>Hello <b>world</b></p>, where "Hello " and "world" are separate text leaves
            // with different computed_properties).
            if (auto properties = parent->computed_properties()) {
                Gfx::Font const* font = nullptr;
                if (auto layout = parent->layout_node()) {
                    font = &layout->first_available_font();
                    node_data.font_family = font->family().to_string();
                }
                node_data.font_size = MUST(String::formatted("{}", properties->font_size().to_double()));
                node_data.font_weight = MUST(String::formatted("{}", properties->font_weight()));

                auto const& font_style_value = properties->property(CSS::PropertyID::FontStyle).as_font_style();
                switch (font_style_value.font_style()) {
                case CSS::FontStyleKeyword::Italic:
                    node_data.font_style = "italic"_string;
                    break;
                case CSS::FontStyleKeyword::Oblique:
                    node_data.font_style = "oblique"_string;
                    break;
                default:
                    node_data.font_style = "normal"_string;
                    break;
                }

                StringBuilder decoration_builder;
                for (auto line : properties->text_decoration_line()) {
                    if (!decoration_builder.is_empty())
                        decoration_builder.append(' ');
                    switch (line) {
                    case CSS::TextDecorationLine::Underline:
                        decoration_builder.append("underline"sv);
                        break;
                    case CSS::TextDecorationLine::LineThrough:
                        decoration_builder.append("line-through"sv);
                        break;
                    case CSS::TextDecorationLine::Overline:
                        decoration_builder.append("overline"sv);
                        break;
                    case CSS::TextDecorationLine::Blink:
                        decoration_builder.append("blink"sv);
                        break;
                    default:
                        break;
                    }
                }
                node_data.text_decoration = decoration_builder.is_empty()
                    ? "none"_string
                    : MUST(decoration_builder.to_string());

                CSS::ColorResolutionContext color_context = CSS::ColorResolutionContext::for_element(
                    DOM::AbstractElement { const_cast<DOM::Element&>(*parent) });
                auto fg = properties->color(CSS::PropertyID::Color, color_context);
                node_data.color = MUST(String::from_byte_string(fg.to_byte_string_without_alpha()));
                auto bg = properties->color(CSS::PropertyID::BackgroundColor, color_context);
                if (bg.alpha() > 0)
                    node_data.background_color = MUST(String::from_byte_string(bg.to_byte_string_without_alpha()));

                // Compute per-character (x, y) offsets and line-break positions. If the text is laid out via
                // Layout::TextNode with PaintableFragments, we use the fragment data for correct wrapping; otherwise,
                // we fall back to a single-line font-metrics computation.
                auto utf8 = node_data.name.bytes_as_string_view();
                Utf8View view { utf8 };

                auto* layout = text_node.layout_node();
                // For inline text, the PaintableFragments we need live on the *containing block's* PaintableWithLines —
                // not the text node's own first_paintable, which usually isn't a PaintableWithLines for inline content.
                // Find that containing block, then keep only fragments whose layout node matches our text node. Hold
                // the RefPtr that backs paintable_with_lines until we're done iterating its fragments.
                RefPtr<Painting::Paintable const> paintable_ref;
                Painting::PaintableWithLines const* paintable_with_lines = nullptr;
                if (layout) {
                    paintable_ref = layout->first_paintable();
                    if (auto* direct = as_if<Painting::PaintableWithLines>(paintable_ref.ptr())) {
                        paintable_with_lines = direct;
                    } else if (auto containing_block = layout->containing_block()) {
                        paintable_ref = containing_block->first_paintable();
                        paintable_with_lines = as_if<Painting::PaintableWithLines>(paintable_ref.ptr());
                    }
                }
                Vector<Painting::PaintableFragment const*> matching_fragments;
                if (paintable_with_lines) {
                    for (auto const& fragment : paintable_with_lines->fragments()) {
                        if (&fragment.layout_node() == layout)
                            matching_fragments.append(&fragment);
                    }
                }
                if (!matching_fragments.is_empty()) {
                    // Use Layout fragments: each fragment corresponds to one visual line of this text node.
                    auto text_bounds_top_left = node_data.bounds.top_left();
                    auto text_utf16 = text_node.data().utf16_view();
                    for (auto const* fragment : matching_fragments) {
                        auto fragment_rect = fragment->absolute_rect();
                        auto line_top_y = static_cast<i32>(fragment_rect.y().to_int() - text_bounds_top_left.y());
                        auto line_left_x = static_cast<i32>(fragment_rect.x().to_int() - text_bounds_top_left.x());
                        auto line_height = static_cast<i32>(fragment_rect.height().to_int());
                        auto fragment_start_utf16 = fragment->start_offset();
                        auto fragment_length_utf16 = fragment->length_in_code_units();
                        auto fragment_character_start = text_utf16.code_point_offset_of(fragment_start_utf16);
                        auto fragment_character_end = text_utf16.code_point_offset_of(fragment_start_utf16 + fragment_length_utf16);
                        node_data.line_break_character_offsets.append(static_cast<i32>(fragment_character_start));
                        node_data.line_heights.append(line_height);
                        // Walk each character in the fragment, accumulating x within the line.
                        float cumulative_x = static_cast<float>(line_left_x);
                        auto utf8_start = view.byte_offset_of(fragment_character_start);
                        auto utf8_end = view.byte_offset_of(fragment_character_end);
                        Utf8View fragment_view { utf8.substring_view(utf8_start, utf8_end - utf8_start) };
                        for (u32 code_point : fragment_view) {
                            node_data.character_offsets.append(Gfx::IntPoint { static_cast<i32>(cumulative_x), line_top_y });
                            cumulative_x += font ? font->glyph_width(code_point) : 0.0f;
                        }
                    }
                } else if (font) {
                    // Fallback: single-line font-metrics computation.
                    node_data.line_break_character_offsets.append(0);
                    node_data.line_heights.append(node_data.bounds.height());
                    float cumulative = 0;
                    for (u32 code_point : view) {
                        node_data.character_offsets.append(Gfx::IntPoint { static_cast<i32>(cumulative), 0 });
                        cumulative += font->glyph_width(code_point);
                    }
                }
            }
        }
    }

    // Populate caret offset and selection range from the DOM Selection — for any node (element or text) that
    // contains it. This is what AtkText::get_caret_offset and get_n_selections report.
    if (auto selection = document.get_selection()) {
        auto* focus_node = selection->focus_node().ptr();
        auto* anchor_node = selection->anchor_node().ptr();
        if (focus_node && focus_node == value().ptr()) {
            // DOM offsets into text nodes are UTF-16 code-unit offsets — but AT-SPI2 expects Unicode character offsets.
            // For ASCII and BMP, those two match; but they diverge for astral-plane code points (emoji, etc.) — where
            // one code point occupies two UTF-16 code units. So, convert using Utf16View::code_point_offset_of.
            auto utf16_offset_to_character_offset = [focus_node](u32 utf16_offset) -> i32 {
                if (!focus_node->is_text())
                    return static_cast<i32>(utf16_offset);
                auto const& text = static_cast<DOM::Text const&>(*focus_node).data();
                return static_cast<i32>(text.utf16_view().code_point_offset_of(utf16_offset));
            };
            node_data.caret_offset = utf16_offset_to_character_offset(selection->focus_offset());
            if (!selection->is_collapsed() && anchor_node == focus_node) {
                auto anchor_character_offset = utf16_offset_to_character_offset(selection->anchor_offset());
                node_data.selection_start = min(node_data.caret_offset, anchor_character_offset);
                node_data.selection_end = max(node_data.caret_offset, anchor_character_offset);
            }
        }
    }

    auto my_id = node_data.id;

    auto should_skip_child = [](AccessibilityTreeNode const& child) -> bool {
        if (child.value()->is_uninteresting_whitespace_node())
            return true;
        if (child.value()->is_text()) {
            for (auto* ancestor = child.value()->parent(); ancestor; ancestor = ancestor->parent()) {
                if (is<HTML::HTMLHeadElement>(*ancestor))
                    return true;
            }
        }
        return false;
    };

    for (auto const& child : children()) {
        if (should_skip_child(*child))
            continue;
        node_data.child_ids.append(static_cast<i64>(child->value()->unique_id().value()));
    }

    out.append(move(node_data));

    for (auto const& child : children()) {
        if (should_skip_child(*child))
            continue;
        child->serialize_tree_as_node_data(out, document, my_id);
    }
}

void AccessibilityTreeNode::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_value);
    visitor.visit(m_children);
}

}
