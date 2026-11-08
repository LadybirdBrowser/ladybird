/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Gamepad/EventNames.h>
#include <LibWeb/HTML/HTMLFrameElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFrameSetElement);

HTMLFrameSetElement::HTMLFrameSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFrameSetElement::~HTMLFrameSetElement() = default;

CSS::ElementBoxKind HTMLFrameSetElement::box_kind() const
{
    return CSS::ElementBoxKind::FrameSet;
}

using Dimension = CSS::StyleEngineFFI::FfiFrameSetDimension;
using DimensionUnit = CSS::StyleEngineFFI::FfiFrameSetDimensionUnit;
using Border = CSS::StyleEngineFFI::FfiFrameSetBorder;

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-a-list-of-dimensions
static Vector<Dimension> parse_a_list_of_dimensions(Utf16View raw_input)
{
    // 1. Let raw input be the string being parsed.
    // 2. If the last character in raw input is a U+002C COMMA character (,), then remove that character from raw input.
    if (!raw_input.is_empty() && raw_input.code_unit_at(raw_input.length_in_code_units() - 1) == ',')
        raw_input = raw_input.substring_view(0, raw_input.length_in_code_units() - 1);

    // 3. Split the string raw input on commas. Let raw tokens be the resulting list of tokens.
    Vector<Utf16View> raw_tokens;
    for (size_t start = 0; start < raw_input.length_in_code_units();) {
        size_t end = start;
        while (end < raw_input.length_in_code_units() && raw_input.code_unit_at(end) != ',')
            ++end;
        raw_tokens.append(raw_input.substring_view(start, end - start).trim_ascii_whitespace());
        start = end + 1;
    }

    // 4. Let result be an empty list of number/unit pairs.
    Vector<Dimension> result;

    // 5. For each token in raw tokens, run the following substeps:
    for (auto input : raw_tokens) {
        // 1. Let input be the token.
        // 2. Let position be a pointer into input, initially pointing at the start of the string.
        size_t position = 0;
        auto const length = input.length_in_code_units();

        // 3. Let value be the number 0.
        double value = 0;

        // 4. Let unit be absolute.
        auto unit = DimensionUnit::Absolute;

        // 5. If position is past the end of input, set unit to relative and jump to the last substep.
        if (position >= length) {
            unit = DimensionUnit::Relative;
        } else {
            // 6. If the character at position is an ASCII digit, collect a sequence of code points that are ASCII digits
            //    from input given position, interpret the resulting sequence as an integer in base ten, and increment
            //    value by that integer.
            while (position < length && is_ascii_digit(input.code_unit_at(position))) {
                value = value * 10 + (input.code_unit_at(position) - '0');
                ++position;
            }

            // 7. If the character at position is U+002E (.):
            if (position < length && input.code_unit_at(position) == '.') {
                ++position;

                // 1. Collect a sequence of code points consisting of ASCII whitespace and ASCII digits from input given
                //    position. Let s be the resulting sequence.
                // 2. Remove all ASCII whitespace in s.
                // 3. If s is not the empty string:
                //    1. Let length be the number of characters in s (after the spaces were removed).
                //    2. Let fraction be the result of interpreting s as a base-ten integer, and then dividing that
                //       number by 10^length.
                //    3. Increment value by fraction.
                double scale = 0.1;
                while (position < length && (is_ascii_digit(input.code_unit_at(position)) || is_ascii_space(input.code_unit_at(position)))) {
                    if (is_ascii_digit(input.code_unit_at(position))) {
                        value += (input.code_unit_at(position) - '0') * scale;
                        scale /= 10;
                    }
                    ++position;
                }
            }

            // 8. Skip ASCII whitespace within input given position.
            while (position < length && is_ascii_space(input.code_unit_at(position)))
                ++position;

            // 9. If the character at position is a U+0025 PERCENT SIGN character (%), then set unit to percentage.
            //    Otherwise, if the character at position is a U+002A ASTERISK character (*), then set unit to relative.
            if (position < length && input.code_unit_at(position) == '%')
                unit = DimensionUnit::Percentage;
            else if (position < length && input.code_unit_at(position) == '*')
                unit = DimensionUnit::Relative;
        }

        // 10. Add an entry to result consisting of the number given by value and the unit given by unit.
        result.append({ .value = value, .unit = unit });
    }

    // 6. Return the list result.
    return result;
}

// https://html.spec.whatwg.org/multipage/rendering.html#has-a-border
static bool has_a_border(DOM::Element const& element)
{
    // 1. If the element has a frameborder attribute whose value is not the empty string and whose first character is
    //    either a U+0031 DIGIT ONE (1) character, a U+0079 LATIN SMALL LETTER Y character (y), or a U+0059 LATIN CAPITAL
    //    LETTER Y character (Y), then return true.
    // 2. Otherwise, if the element has a frameborder attribute, return false.
    auto frameborder = element.attribute(AttributeNames::frameborder);

    // INTEROP: On a frameset element, other browsers treat every value but "yes", "1", "no" and "0" as a missing attribute.
    if (is<HTMLFrameSetElement>(element) && frameborder.has_value()) {
        auto value = frameborder->utf16_view();
        if (!value.equals_ignoring_ascii_case("yes"sv) && !value.equals_ignoring_ascii_case("no"sv) && value != "1"sv && value != "0"sv)
            frameborder.clear();
    }

    if (frameborder.has_value()) {
        auto value = frameborder->utf16_view();
        return !value.is_empty() && first_is_one_of(value.code_unit_at(0), '1', 'y', 'Y');
    }

    // 3. Otherwise, if the element has a parent element that is a frameset element, then return true if that element
    //    has a border, and false if it does not.
    if (auto const* parent_frameset = as_if<HTMLFrameSetElement>(element.parent()))
        return has_a_border(*parent_frameset);

    // 4. Otherwise, return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/rendering.html#frame-border-colour
static Color frame_border_color(DOM::Element const& element)
{
    // 1. If the element has a bordercolor attribute, and applying the rules for parsing a legacy color value to that
    //    attribute's value does not return failure, then return the color so obtained.
    if (auto bordercolor = element.attribute(AttributeNames::bordercolor); bordercolor.has_value()) {
        if (auto color = parse_legacy_color_value(bordercolor->utf16_view()); color.has_value())
            return *color;
    }

    // 2. Otherwise, if the element has a parent element that is a frameset element, then return the frame border color
    //    of that element.
    if (auto const* parent_frameset = as_if<HTMLFrameSetElement>(element.parent()))
        return frame_border_color(*parent_frameset);

    // 3. Otherwise, return gray.
    return Color { 128, 128, 128 };
}

// INTEROP: The specification does not define how thick a border is. Other browsers default to 6 pixels, honor a border
//          attribute that a nested frameset inherits, and make a border with a negative thickness 1 pixel thick.
u32 HTMLFrameSetElement::frame_border_thickness() const
{
    static constexpr u32 default_border_thickness = 6;

    if (!has_a_border(*this))
        return 0;
    if (auto border = attribute(AttributeNames::border); border.has_value()) {
        auto thickness = parse_integer(border->utf16_view()).value_or(0);
        return thickness < 0 ? 1 : static_cast<u32>(thickness);
    }
    if (auto const* parent_frameset = as_if<HTMLFrameSetElement>(parent()))
        return parent_frameset->frame_border_thickness();
    return default_border_thickness;
}

// https://html.spec.whatwg.org/multipage/rendering.html#frames-and-framesets
void HTMLFrameSetElement::publish_layout_data() const
{
    if (style_node_id().value() == 0)
        return;

    // 1. The cols and rows variables are lists of zero or more pairs consisting of a number and a unit, the unit being
    //    one of percentage, relative, and absolute.
    //    Use the rules for parsing a list of dimensions to parse the value of the element's cols attribute, if there is
    //    one. Let cols be the result, or an empty list if there is no such attribute.
    //    Use the rules for parsing a list of dimensions to parse the value of the element's rows attribute, if there is
    //    one. Let rows be the result, or an empty list if there is no such attribute.
    auto parse_attribute = [&](Utf16FlyString const& name) {
        auto value = attribute(name);
        return value.has_value() ? parse_a_list_of_dimensions(value->utf16_view()) : Vector<Dimension> {};
    };
    auto cols = parse_attribute(AttributeNames::cols);
    auto rows = parse_attribute(AttributeNames::rows);

    auto finish_list = [](Vector<Dimension>& list) {
        // 2. For any of the entries in cols or rows that have the number zero and the unit relative, change the entry's
        //    number to one.
        for (auto& entry : list) {
            if (entry.value == 0 && entry.unit == DimensionUnit::Relative)
                entry.value = 1;
        }

        // 3. If cols has no entries, then add a single entry consisting of the value 1 and the unit relative to cols.
        //    If rows has no entries, then add a single entry consisting of the value 1 and the unit relative to rows.
        if (list.is_empty())
            list.append({ .value = 1, .unit = DimensionUnit::Relative });
    };
    finish_list(cols);
    finish_list(rows);

    // If the frameset element has a border, draw an outer set of borders around the rectangles, using the element's
    // frame border color.
    // INTEROP: Other browsers do not draw an outer set of borders.

    // For each rectangle, if there is an element assigned to that rectangle, and that element has a border, draw an
    // inner set of borders around that rectangle, using the element's frame border color.
    // NB: Neighboring rectangles share the border between them, so this records which of the borders between tracks are
    //     drawn. Where two elements have a border in common, the color of the later one is used.
    Vector<Border> borders;
    borders.resize(cols.size() + rows.size());
    auto column_borders = borders.span().slice(0, cols.size());
    auto row_borders = borders.span().slice(cols.size());
    size_t child_index = 0;
    for_each_child_of_type<HTMLElement>([&](HTMLElement const& child) {
        if (!is<HTMLFrameElement>(child) && !is<HTMLFrameSetElement>(child))
            return IterationDecision::Continue;
        auto column = child_index % cols.size();
        auto row = child_index / cols.size();
        if (row >= rows.size())
            return IterationDecision::Break;
        if (has_a_border(child)) {
            auto const color = frame_border_color(child).value();
            auto draw = [color](Border& border) {
                border.is_drawn = true;
                border.color = color;
            };
            if (column > 0)
                draw(column_borders[column - 1]);
            if (column + 1 < cols.size())
                draw(column_borders[column]);
            if (row > 0)
                draw(row_borders[row - 1]);
            if (row + 1 < rows.size())
                draw(row_borders[row]);
        }
        ++child_index;
        return IterationDecision::Continue;
    });

    CSS::StyleEngineFFI::style_engine_set_element_frameset_layout(
        document().style_computer().style_engine().host(),
        style_node_id(),
        cols.span(),
        rows.span(),
        borders.span(),
        frame_border_thickness());
}

void HTMLFrameSetElement::own_layout_data_changed()
{
    publish_layout_data();
    Layout::ForcedReadScope read { document() };
    if (auto* layout_node = unsafe_layout_node(read))
        Layout::RustFFI::render_state_restamp_frameset_layout(layout_node->document_host(), Layout::Node::slot_id(layout_node));
}

void HTMLFrameSetElement::layout_data_changed()
{
    own_layout_data_changed();

    // NB: A nested frameset inherits whether it has a border, and the thickness and color of its borders.
    for_each_child_of_type<HTMLFrameSetElement>([](HTMLFrameSetElement& child) {
        child.layout_data_changed();
        return IterationDecision::Continue;
    });
}

void HTMLFrameSetElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    own_layout_data_changed();

    // NB: Only a frameset that just arrived can have inherited something else.
    if (auto* frameset = as_if<HTMLFrameSetElement>(*metadata.node); frameset && metadata.type == ChildrenChangedMetadata::Type::Inserted)
        frameset->layout_data_changed();
}

void HTMLFrameSetElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)               \
    if (name == HTML::AttributeNames::attribute_name) {       \
        element_event_handler_attribute_changed(name, value); \
    }
    ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

    if (!namespace_.has_value() && first_is_one_of(name, AttributeNames::cols, AttributeNames::rows, AttributeNames::frameborder, AttributeNames::border, AttributeNames::bordercolor)) {
        layout_data_changed();

        // NB: A parent frameset draws the borders of this frameset's rectangle from its frameborder and bordercolor.
        if (first_is_one_of(name, AttributeNames::frameborder, AttributeNames::bordercolor)) {
            if (auto* parent_frameset = as_if<HTMLFrameSetElement>(parent()))
                parent_frameset->own_layout_data_changed();
        }
    }
}

GC::Ptr<DOM::EventTarget> HTMLFrameSetElement::global_event_handlers_to_event_target(Utf16FlyString const& event_name)
{
    // NOTE: This is a little weird, but IIUC document.body.onload actually refers to window.onload
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    if (DOM::is_window_reflecting_body_element_event_handler(event_name))
        return document().window();

    return *this;
}

GC::Ptr<DOM::EventTarget> HTMLFrameSetElement::window_event_handlers_to_event_target()
{
    // All WindowEventHandlers on HTMLFrameSetElement (e.g. document.body.onrejectionhandled) are mapped to window.on{event}.
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    return document().window();
}

}
