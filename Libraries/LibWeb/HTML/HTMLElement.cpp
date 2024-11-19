/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLElementPrototype.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/LiveNodeList.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/ElementInternals.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLElement);

HTMLElement::HTMLElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : Element(document, move(qualified_name))
{
}

HTMLElement::~HTMLElement() = default;

void HTMLElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLElement);
}

void HTMLElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    HTMLOrSVGElement::visit_edges(visitor);
    visitor.visit(m_labels);
    visitor.visit(m_attached_internals);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-dir
StringView HTMLElement::dir() const
{
    // FIXME: This should probably be `Reflect` in the IDL.
    // The dir IDL attribute on an element must reflect the dir content attribute of that element, limited to only known values.
    auto dir = get_attribute_value(HTML::AttributeNames::dir);
#define __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(keyword) \
    if (dir.equals_ignoring_ascii_case(#keyword##sv))   \
        return #keyword##sv;
    ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTES
#undef __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE

    return {};
}

void HTMLElement::set_dir(String const& dir)
{
    MUST(set_attribute(HTML::AttributeNames::dir, dir));
}

bool HTMLElement::is_editable() const
{
    switch (m_content_editable_state) {
    case ContentEditableState::True:
        return true;
    case ContentEditableState::False:
        return false;
    case ContentEditableState::Inherit:
        return parent() && parent()->is_editable();
    default:
        VERIFY_NOT_REACHED();
    }
}

bool HTMLElement::is_focusable() const
{
    return m_content_editable_state == ContentEditableState::True;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-iscontenteditable
bool HTMLElement::is_content_editable() const
{
    // The isContentEditable IDL attribute, on getting, must return true if the element is either an editing host or
    // editable, and false otherwise.
    return is_editable();
}

StringView HTMLElement::content_editable() const
{
    switch (m_content_editable_state) {
    case ContentEditableState::True:
        return "true"sv;
    case ContentEditableState::False:
        return "false"sv;
    case ContentEditableState::Inherit:
        return "inherit"sv;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/interaction.html#contenteditable
WebIDL::ExceptionOr<void> HTMLElement::set_content_editable(StringView content_editable)
{
    if (content_editable.equals_ignoring_ascii_case("inherit"sv)) {
        remove_attribute(HTML::AttributeNames::contenteditable);
        return {};
    }
    if (content_editable.equals_ignoring_ascii_case("true"sv)) {
        MUST(set_attribute(HTML::AttributeNames::contenteditable, "true"_string));
        return {};
    }
    if (content_editable.equals_ignoring_ascii_case("false"sv)) {
        MUST(set_attribute(HTML::AttributeNames::contenteditable, "false"_string));
        return {};
    }
    return WebIDL::SyntaxError::create(realm(), "Invalid contentEditable value, must be 'true', 'false', or 'inherit'"_string);
}

// https://html.spec.whatwg.org/multipage/dom.html#set-the-inner-text-steps
void HTMLElement::set_inner_text(StringView text)
{
    // 1. Let fragment be the rendered text fragment for value given element's node document.
    auto fragment = rendered_text_fragment(text);

    // 2. Replace all with fragment within element.
    replace_all(fragment);

    set_needs_style_update(true);
}

// https://html.spec.whatwg.org/multipage/dom.html#merge-with-the-next-text-node
static void merge_with_the_next_text_node(DOM::Text& node)
{
    // 1. Let next be node's next sibling.
    auto next = node.next_sibling();

    // 2. If next is not a Text node, then return.
    if (!is<DOM::Text>(next))
        return;

    // 3. Replace data with node, node's data's length, 0, and next's data.
    MUST(node.replace_data(node.length_in_utf16_code_units(), 0, static_cast<DOM::Text const&>(*next).data()));

    // 4. Remove next.
    next->remove();
}

// https://html.spec.whatwg.org/multipage/dom.html#the-innertext-idl-attribute:dom-outertext-2
WebIDL::ExceptionOr<void> HTMLElement::set_outer_text(String const& value)
{
    // 1. If this's parent is null, then throw a "NoModificationAllowedError" DOMException.
    if (!parent())
        return WebIDL::NoModificationAllowedError::create(realm(), "setOuterText: parent is null"_string);

    // 2. Let next be this's next sibling.
    auto* next = next_sibling();

    // 3. Let previous be this's previous sibling.
    auto* previous = previous_sibling();

    // 4. Let fragment be the rendered text fragment for the given value given this's node document.
    auto fragment = rendered_text_fragment(value);

    // 5. If fragment has no children, then append a new Text node whose data is the empty string and node document is this's node document to fragment.
    if (!fragment->has_children())
        MUST(fragment->append_child(document().create_text_node(String {})));

    // 6. Replace this with fragment within this's parent.
    MUST(parent()->replace_child(fragment, *this));

    // 7. If next is non-null and next's previous sibling is a Text node, then merge with the next text node given next's previous sibling.
    if (next && is<DOM::Text>(next->previous_sibling()))
        merge_with_the_next_text_node(static_cast<DOM::Text&>(*next->previous_sibling()));

    // 8. If previous is a Text node, then merge with the next text node given previous.
    if (is<DOM::Text>(previous))
        merge_with_the_next_text_node(static_cast<DOM::Text&>(*previous));

    set_needs_style_update(true);
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#rendered-text-fragment
GC::Ref<DOM::DocumentFragment> HTMLElement::rendered_text_fragment(StringView input)
{
    // 1. Let fragment be a new DocumentFragment whose node document is document.
    //    Instead of creating a DocumentFragment the nodes are appended directly.
    auto fragment = realm().create<DOM::DocumentFragment>(document());

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    // 3. Let text be the empty string.
    // 4. While position is not past the end of input:
    while (!input.is_empty()) {
        // 1. Collect a sequence of code points that are not U+000A LF or U+000D CR from input given position, and set text to the result.
        auto newline_index = input.find_any_of("\n\r"sv);
        size_t const sequence_end_index = newline_index.value_or(input.length());
        StringView const text = input.substring_view(0, sequence_end_index);
        input = input.substring_view_starting_after_substring(text);

        // 2. If text is not the empty string, then append a new Text node whose data is text and node document is document to fragment.
        if (!text.is_empty()) {
            MUST(fragment->append_child(document().create_text_node(MUST(String::from_utf8(text)))));
        }

        // 3. While position is not past the end of input, and the code point at position is either U+000A LF or U+000D CR:
        while (input.starts_with('\n') || input.starts_with('\r')) {
            // 1. If the code point at position is U+000D CR and the next code point is U+000A LF, then advance position to the next code point in input.
            if (input.starts_with("\r\n"sv)) {
                // 2. Advance position to the next code point in input.
                input = input.substring_view(2);
            } else {
                // 2. Advance position to the next code point in input.
                input = input.substring_view(1);
            }

            // 3. Append the result of creating an element given document, br, and the HTML namespace to fragment.
            auto br_element = DOM::create_element(document(), HTML::TagNames::br, Namespace::HTML).release_value();
            MUST(fragment->append_child(br_element));
        }
    }

    // 5. Return fragment.
    return fragment;
}

struct RequiredLineBreakCount {
    int count { 0 };
};

// https://html.spec.whatwg.org/multipage/dom.html#rendered-text-collection-steps
static Vector<Variant<String, RequiredLineBreakCount>> rendered_text_collection_steps(DOM::Node const& node)
{
    // 1. Let items be the result of running the rendered text collection steps with each child node of node in tree order, and then concatenating the results to a single list.
    Vector<Variant<String, RequiredLineBreakCount>> items;
    node.for_each_child([&](auto const& child) {
        auto child_items = rendered_text_collection_steps(child);
        items.extend(move(child_items));
        return IterationDecision::Continue;
    });

    // NOTE: Steps are re-ordered here a bit.

    // 3. If node is not being rendered, then return items.
    //    For the purpose of this step, the following elements must act as described
    //    if the computed value of the 'display' property is not 'none':
    //    FIXME: - select elements have an associated non-replaced inline CSS box whose child boxes include only those of optgroup and option element child nodes;
    //    FIXME: - optgroup elements have an associated non-replaced block-level CSS box whose child boxes include only those of option element child nodes; and
    //    FIXME: - option element have an associated non-replaced block-level CSS box whose child boxes are as normal for non-replaced block-level CSS boxes.
    auto* layout_node = node.layout_node();
    if (!layout_node)
        return items;

    auto const& computed_values = layout_node->computed_values();

    // 2. If node's computed value of 'visibility' is not 'visible', then return items.
    if (computed_values.visibility() != CSS::Visibility::Visible)
        return items;

    // AD-HOC: If node's computed value of 'content-visibility' is 'hidden', then return items.
    if (computed_values.content_visibility() == CSS::ContentVisibility::Hidden)
        return items;

    // 4. If node is a Text node, then for each CSS text box produced by node, in content order,
    //    compute the text of the box after application of the CSS 'white-space' processing rules
    //    and 'text-transform' rules, set items to the list of the resulting strings, and return items.

    //    FIXME: The CSS 'white-space' processing rules are slightly modified:
    //           collapsible spaces at the end of lines are always collapsed,
    //           but they are only removed if the line is the last line of the block,
    //           or it ends with a br element. Soft hyphens should be preserved. [CSSTEXT]

    if (is<DOM::Text>(node)) {
        auto const* layout_text_node = verify_cast<Layout::TextNode>(layout_node);
        items.append(layout_text_node->text_for_rendering());
        return items;
    }

    // 5. If node is a br element, then append a string containing a single U+000A LF code point to items.
    if (is<HTML::HTMLBRElement>(node)) {
        items.append("\n"_string);
        return items;
    }

    auto display = computed_values.display();

    // 6. If node's computed value of 'display' is 'table-cell', and node's CSS box is not the last 'table-cell' box of its enclosing 'table-row' box, then append a string containing a single U+0009 TAB code point to items.
    if (display.is_table_cell() && node.next_sibling())
        items.append("\t"_string);

    // 7. If node's computed value of 'display' is 'table-row', and node's CSS box is not the last 'table-row' box of the nearest ancestor 'table' box, then append a string containing a single U+000A LF code point to items.
    if (display.is_table_row() && node.next_sibling())
        items.append("\n"_string);

    // 8. If node is a p element, then append 2 (a required line break count) at the beginning and end of items.
    if (is<HTML::HTMLParagraphElement>(node)) {
        items.prepend(RequiredLineBreakCount { 2 });
        items.append(RequiredLineBreakCount { 2 });
    }

    // 9. If node's used value of 'display' is block-level or 'table-caption', then append 1 (a required line break count) at the beginning and end of items. [CSSDISPLAY]
    if (display.is_block_outside() || display.is_table_caption()) {
        items.prepend(RequiredLineBreakCount { 1 });
        items.append(RequiredLineBreakCount { 1 });
    }

    // 10. Return items.
    return items;
}

// https://html.spec.whatwg.org/multipage/dom.html#get-the-text-steps
String HTMLElement::get_the_text_steps()
{
    // 1. If element is not being rendered or if the user agent is a non-CSS user agent, then return element's descendant text content.
    document().update_layout();
    if (!layout_node())
        return descendant_text_content();

    // 2. Let results be a new empty list.
    Vector<Variant<String, RequiredLineBreakCount>> results;

    // 3. For each child node node of element:
    for_each_child([&](Node const& node) {
        // 1. Let current be the list resulting in running the rendered text collection steps with node.
        //    Each item in results will either be a string or a positive integer (a required line break count).
        auto current = rendered_text_collection_steps(node);

        // 2. For each item item in current, append item to results.
        results.extend(move(current));
        return IterationDecision::Continue;
    });

    // 4. Remove any items from results that are the empty string.
    results.remove_all_matching([](auto& item) {
        return item.visit(
            [](String const& string) { return string.is_empty(); },
            [](RequiredLineBreakCount const&) { return false; });
    });

    // 5. Remove any runs of consecutive required line break count items at the start or end of results.
    while (!results.is_empty() && results.first().has<RequiredLineBreakCount>())
        results.take_first();
    while (!results.is_empty() && results.last().has<RequiredLineBreakCount>())
        results.take_last();

    // 6. Replace each remaining run of consecutive required line break count items
    //    with a string consisting of as many U+000A LF code points as the maximum of the values
    //    in the required line break count items.
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].has<RequiredLineBreakCount>())
            continue;

        int max_line_breaks = results[i].get<RequiredLineBreakCount>().count;
        size_t j = i + 1;
        while (j < results.size() && results[j].has<RequiredLineBreakCount>()) {
            max_line_breaks = max(max_line_breaks, results[j].get<RequiredLineBreakCount>().count);
            ++j;
        }

        results.remove(i, j - i);
        results.insert(i, MUST(String::repeated('\n', max_line_breaks)));
    }

    // 7. Return the concatenation of the string items in results.
    StringBuilder builder;
    for (auto& item : results) {
        item.visit(
            [&](String const& string) { builder.append(string); },
            [&](RequiredLineBreakCount const&) {});
    }
    return builder.to_string_without_validation();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-innertext
String HTMLElement::inner_text()
{
    // The innerText and outerText getter steps are to return the result of running get the text steps with this.
    return get_the_text_steps();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-outertext
String HTMLElement::outer_text()
{
    // The innerText and outerText getter steps are to return the result of running get the text steps with this.
    return get_the_text_steps();
}

// https://www.w3.org/TR/cssom-view-1/#dom-htmlelement-offsetparent
GC::Ptr<DOM::Element> HTMLElement::offset_parent() const
{
    const_cast<DOM::Document&>(document()).update_layout();

    // 1. If any of the following holds true return null and terminate this algorithm:
    //    - The element does not have an associated CSS layout box.
    //    - The element is the root element.
    //    - The element is the HTML body element.
    //    - The element’s computed value of the position property is fixed.
    if (!layout_node())
        return nullptr;
    if (is_document_element())
        return nullptr;
    if (is<HTML::HTMLBodyElement>(*this))
        return nullptr;
    if (layout_node()->is_fixed_position())
        return nullptr;

    // 2. Return the nearest ancestor element of the element for which at least one of the following is true
    //    and terminate this algorithm if such an ancestor is found:
    //    - The computed value of the position property is not static.
    //    - It is the HTML body element.
    //    - The computed value of the position property of the element is static
    //      and the ancestor is one of the following HTML elements: td, th, or table.

    for (auto* ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
        if (!ancestor->layout_node())
            continue;
        if (ancestor->layout_node()->is_positioned())
            return const_cast<Element*>(ancestor);
        if (is<HTML::HTMLBodyElement>(*ancestor))
            return const_cast<Element*>(ancestor);
        if (!ancestor->layout_node()->is_positioned() && ancestor->local_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::table))
            return const_cast<Element*>(ancestor);
    }

    // 3. Return null.
    return nullptr;
}

// https://www.w3.org/TR/cssom-view-1/#dom-htmlelement-offsettop
int HTMLElement::offset_top() const
{
    // 1. If the element is the HTML body element or does not have any associated CSS layout box
    //    return zero and terminate this algorithm.
    if (is<HTML::HTMLBodyElement>(*this))
        return 0;

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<DOM::Document&>(document()).update_layout();

    if (!layout_node())
        return 0;

    CSSPixels top_border_edge_of_element;
    if (paintable()->is_paintable_box()) {
        top_border_edge_of_element = paintable_box()->absolute_border_box_rect().y();
    } else {
        top_border_edge_of_element = paintable()->box_type_agnostic_position().y();
    }

    // 2. If the offsetParent of the element is null
    //    return the y-coordinate of the top border edge of the first CSS layout box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors, and terminate this algorithm.
    auto offset_parent = this->offset_parent();
    if (!offset_parent || !offset_parent->layout_node()) {
        return top_border_edge_of_element.to_int();
    }

    // 3. Return the result of subtracting the y-coordinate of the top padding edge
    //    of the first box associated with the offsetParent of the element
    //    from the y-coordinate of the top border edge of the first box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors.

    // NOTE: We give special treatment to the body element to match other browsers.
    //       Spec bug: https://github.com/w3c/csswg-drafts/issues/10549

    CSSPixels top_padding_edge_of_offset_parent;
    if (offset_parent->is_html_body_element() && !offset_parent->paintable()->is_positioned()) {
        top_padding_edge_of_offset_parent = 0;
    } else if (offset_parent->paintable()->is_paintable_box()) {
        top_padding_edge_of_offset_parent = offset_parent->paintable_box()->absolute_padding_box_rect().y();
    } else {
        top_padding_edge_of_offset_parent = offset_parent->paintable()->box_type_agnostic_position().y();
    }
    return (top_border_edge_of_element - top_padding_edge_of_offset_parent).to_int();
}

// https://www.w3.org/TR/cssom-view-1/#dom-htmlelement-offsetleft
int HTMLElement::offset_left() const
{
    // 1. If the element is the HTML body element or does not have any associated CSS layout box return zero and terminate this algorithm.
    if (is<HTML::HTMLBodyElement>(*this))
        return 0;

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<DOM::Document&>(document()).update_layout();

    if (!layout_node())
        return 0;

    CSSPixels left_border_edge_of_element;
    if (paintable()->is_paintable_box()) {
        left_border_edge_of_element = paintable_box()->absolute_border_box_rect().x();
    } else {
        left_border_edge_of_element = paintable()->box_type_agnostic_position().x();
    }

    // 2. If the offsetParent of the element is null
    //    return the x-coordinate of the left border edge of the first CSS layout box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors, and terminate this algorithm.
    auto offset_parent = this->offset_parent();
    if (!offset_parent || !offset_parent->layout_node()) {
        return left_border_edge_of_element.to_int();
    }

    // 3. Return the result of subtracting the x-coordinate of the left padding edge
    //    of the first CSS layout box associated with the offsetParent of the element
    //    from the x-coordinate of the left border edge of the first CSS layout box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors.

    // NOTE: We give special treatment to the body element to match other browsers.
    //       Spec bug: https://github.com/w3c/csswg-drafts/issues/10549

    CSSPixels left_padding_edge_of_offset_parent;
    if (offset_parent->is_html_body_element() && !offset_parent->paintable()->is_positioned()) {
        left_padding_edge_of_offset_parent = 0;
    } else if (offset_parent->paintable()->is_paintable_box()) {
        left_padding_edge_of_offset_parent = offset_parent->paintable_box()->absolute_padding_box_rect().x();
    } else {
        left_padding_edge_of_offset_parent = offset_parent->paintable()->box_type_agnostic_position().x();
    }
    return (left_border_edge_of_element - left_padding_edge_of_offset_parent).to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-htmlelement-offsetwidth
int HTMLElement::offset_width() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<DOM::Document&>(document()).update_layout();

    // 1. If the element does not have any associated CSS layout box return zero and terminate this algorithm.
    if (!paintable_box())
        return 0;

    // 2. Return the width of the axis-aligned bounding box of the border boxes of all fragments generated by the element’s principal box,
    //    ignoring any transforms that apply to the element and its ancestors.
    // FIXME: Account for inline boxes.
    return paintable_box()->border_box_width().to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-htmlelement-offsetheight
int HTMLElement::offset_height() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<DOM::Document&>(document()).update_layout();

    // 1. If the element does not have any associated CSS layout box return zero and terminate this algorithm.
    if (!paintable_box())
        return 0;

    // 2. Return the height of the axis-aligned bounding box of the border boxes of all fragments generated by the element’s principal box,
    //    ignoring any transforms that apply to the element and its ancestors.
    // FIXME: Account for inline boxes.
    return paintable_box()->border_box_height().to_int();
}

// https://html.spec.whatwg.org/multipage/links.html#cannot-navigate
bool HTMLElement::cannot_navigate() const
{
    // An element element cannot navigate if one of the following is true:

    // - element's node document is not fully active
    if (!document().is_fully_active())
        return true;

    // - element is not an a element and is not connected.
    return !is<HTML::HTMLAnchorElement>(this) && !is_connected();
}

void HTMLElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    HTMLOrSVGElement::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::contenteditable) {
        if (!value.has_value()) {
            m_content_editable_state = ContentEditableState::Inherit;
        } else {
            if (value->is_empty() || value->equals_ignoring_ascii_case("true"sv)) {
                // "true", an empty string or a missing value map to the "true" state.
                m_content_editable_state = ContentEditableState::True;
            } else if (value->equals_ignoring_ascii_case("false"sv)) {
                // "false" maps to the "false" state.
                m_content_editable_state = ContentEditableState::False;
            } else {
                // Having no such attribute or an invalid value maps to the "inherit" state.
                m_content_editable_state = ContentEditableState::Inherit;
            }
        }
    }

    // 1. If namespace is not null, or localName is not the name of an event handler content attribute on element, then return.
    // FIXME: Add the namespace part once we support attribute namespaces.
#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                     \
    if (name == HTML::AttributeNames::attribute_name) {             \
        element_event_handler_attribute_changed(event_name, value); \
    }
    ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
}

WebIDL::ExceptionOr<void> HTMLElement::cloned(Web::DOM::Node& copy, bool clone_children)
{
    TRY(Base::cloned(copy, clone_children));
    TRY(HTMLOrSVGElement::cloned(copy, clone_children));
    return {};
}

void HTMLElement::inserted()
{
    Base::inserted();
    HTMLOrSVGElement::inserted();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#fire-a-synthetic-pointer-event
bool HTMLElement::fire_a_synthetic_pointer_event(FlyString const& type, DOM::Element& target, bool not_trusted)
{
    // 1. Let event be the result of creating an event using PointerEvent.
    // 2. Initialize event's type attribute to e.
    auto event = UIEvents::PointerEvent::create(realm(), type);

    // 3. Initialize event's bubbles and cancelable attributes to true.
    event->set_bubbles(true);
    event->set_cancelable(true);

    // 4. Set event's composed flag.
    event->set_composed(true);

    // 5. If the not trusted flag is set, initialize event's isTrusted attribute to false.
    if (not_trusted) {
        event->set_is_trusted(false);
    }

    // FIXME: 6. Initialize event's ctrlKey, shiftKey, altKey, and metaKey attributes according to the current state
    //           of the key input device, if any (false for any keys that are not available).

    // FIXME: 7. Initialize event's view attribute to target's node document's Window object, if any, and null otherwise.

    // FIXME: 8. event's getModifierState() method is to return values appropriately describing the current state of the key input device.

    // 9. Return the result of dispatching event at target.
    return target.dispatch_event(event);
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-lfe-labels-dev
GC::Ptr<DOM::NodeList> HTMLElement::labels()
{
    // Labelable elements and all input elements have a live NodeList object associated with them that represents the list of label elements, in tree order,
    // whose labeled control is the element in question. The labels IDL attribute of labelable elements that are not form-associated custom elements,
    // and the labels IDL attribute of input elements, on getting, must return that NodeList object, and that same value must always be returned,
    // unless this element is an input element whose type attribute is in the Hidden state, in which case it must instead return null.
    if (!is_labelable())
        return {};

    if (!m_labels) {
        m_labels = DOM::LiveNodeList::create(realm(), root(), DOM::LiveNodeList::Scope::Descendants, [&](auto& node) {
            return is<HTMLLabelElement>(node) && verify_cast<HTMLLabelElement>(node).control() == this;
        });
    }

    return m_labels;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-click
void HTMLElement::click()
{
    // 1. If this element is a form control that is disabled, then return.
    if (auto* form_control = dynamic_cast<FormAssociatedElement*>(this)) {
        if (!form_control->enabled())
            return;
    }

    // 2. If this element's click in progress flag is set, then return.
    if (m_click_in_progress)
        return;

    // 3. Set this element's click in progress flag.
    m_click_in_progress = true;

    // 4. Fire a synthetic pointer event named click at this element, with the not trusted flag set.
    fire_a_synthetic_pointer_event(HTML::EventNames::click, *this, true);

    // 5. Unset this element's click in progress flag.
    m_click_in_progress = false;
}

Optional<ARIA::Role> HTMLElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-address
    if (local_name() == TagNames::address)
        return ARIA::Role::group;
    // https://www.w3.org/TR/html-aria/#el-article
    if (local_name() == TagNames::article)
        return ARIA::Role::article;
    // https://www.w3.org/TR/html-aria/#el-aside
    if (local_name() == TagNames::aside)
        return ARIA::Role::complementary;
    // https://www.w3.org/TR/html-aria/#el-b
    if (local_name() == TagNames::b)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-bdi
    if (local_name() == TagNames::bdi)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-bdo
    if (local_name() == TagNames::bdo)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-code
    if (local_name() == TagNames::code)
        return ARIA::Role::code;
    // https://www.w3.org/TR/html-aria/#el-dfn
    if (local_name() == TagNames::dfn)
        return ARIA::Role::term;
    // https://www.w3.org/TR/html-aria/#el-em
    if (local_name() == TagNames::em)
        return ARIA::Role::emphasis;
    // https://www.w3.org/TR/html-aria/#el-figure
    if (local_name() == TagNames::figure)
        return ARIA::Role::figure;
    // https://www.w3.org/TR/html-aria/#el-footer
    if (local_name() == TagNames::footer) {
        // TODO: If not a descendant of an article, aside, main, nav or section element, or an element with role=article, complementary, main, navigation or region then role=contentinfo
        // Otherwise, role=generic
        return ARIA::Role::generic;
    }
    // https://www.w3.org/TR/html-aria/#el-header
    if (local_name() == TagNames::header) {
        // TODO: If not a descendant of an article, aside, main, nav or section element, or an element with role=article, complementary, main, navigation or region then role=banner
        // Otherwise, role=generic
        return ARIA::Role::generic;
    }
    // https://www.w3.org/TR/html-aria/#el-hgroup
    if (local_name() == TagNames::hgroup)
        return ARIA::Role::group;
    // https://www.w3.org/TR/html-aria/#el-i
    if (local_name() == TagNames::i)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-main
    if (local_name() == TagNames::main)
        return ARIA::Role::main;
    // https://www.w3.org/TR/html-aria/#el-nav
    if (local_name() == TagNames::nav)
        return ARIA::Role::navigation;
    // https://www.w3.org/TR/html-aria/#el-s
    if (local_name() == TagNames::s)
        return ARIA::Role::deletion;
    // https://www.w3.org/TR/html-aria/#el-samp
    if (local_name() == TagNames::samp)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-search
    if (local_name() == TagNames::search)
        return ARIA::Role::search;
    // https://www.w3.org/TR/html-aria/#el-section
    if (local_name() == TagNames::section) {
        // TODO:  role=region if the section element has an accessible name
        //        Otherwise, no corresponding role
        return ARIA::Role::region;
    }
    // https://www.w3.org/TR/html-aria/#el-small
    if (local_name() == TagNames::small)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-strong
    if (local_name() == TagNames::strong)
        return ARIA::Role::strong;
    // https://www.w3.org/TR/html-aria/#el-sub
    if (local_name() == TagNames::sub)
        return ARIA::Role::subscript;
    // https://www.w3.org/TR/html-aria/#el-summary
    if (local_name() == TagNames::summary)
        return ARIA::Role::button;
    // https://www.w3.org/TR/html-aria/#el-sup
    if (local_name() == TagNames::sup)
        return ARIA::Role::superscript;
    // https://www.w3.org/TR/html-aria/#el-u
    if (local_name() == TagNames::u)
        return ARIA::Role::generic;

    return {};
}

// https://html.spec.whatwg.org/multipage/semantics.html#get-an-element's-target
String HTMLElement::get_an_elements_target() const
{
    // To get an element's target, given an a, area, or form element element, run these steps:

    // 1. If element has a target attribute, then return that attribute's value.
    auto maybe_target = attribute(AttributeNames::target);
    if (maybe_target.has_value())
        return maybe_target.release_value();

    // FIXME: 2. If element's node document contains a base element with a
    // target attribute, then return the value of the target attribute of the
    // first such base element.

    // 3. Return the empty string.
    return String {};
}

// https://html.spec.whatwg.org/multipage/links.html#get-an-element's-noopener
TokenizedFeature::NoOpener HTMLElement::get_an_elements_noopener(StringView target) const
{
    // To get an element's noopener, given an a, area, or form element element and a string target:
    auto rel = MUST(get_attribute_value(HTML::AttributeNames::rel).to_lowercase());
    auto link_types = rel.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

    // 1. If element's link types include the noopener or noreferrer keyword, then return true.
    if (link_types.contains_slow("noopener"sv) || link_types.contains_slow("noreferrer"sv))
        return TokenizedFeature::NoOpener::Yes;

    // 2. If element's link types do not include the opener keyword and
    //    target is an ASCII case-insensitive match for "_blank", then return true.
    if (!link_types.contains_slow("opener"sv) && Infra::is_ascii_case_insensitive_match(target, "_blank"sv))
        return TokenizedFeature::NoOpener::Yes;

    // 3. Return false.
    return TokenizedFeature::NoOpener::No;
}

WebIDL::ExceptionOr<GC::Ref<ElementInternals>> HTMLElement::attach_internals()
{
    // 1. If this's is value is not null, then throw a "NotSupportedError" DOMException.
    if (is_value().has_value())
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals cannot be attached to a customized build-in element"_string);

    // 2. Let definition be the result of looking up a custom element definition given this's node document, its namespace, its local name, and null as the is value.
    auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), is_value());

    // 3. If definition is null, then throw an "NotSupportedError" DOMException.
    if (!definition)
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals cannot be attached to an element that is not a custom element"_string);

    // 4. If definition's disable internals is true, then throw a "NotSupportedError" DOMException.
    if (definition->disable_internals())
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals are disabled for this custom element"_string);

    // 5. If this's attached internals is non-null, then throw an "NotSupportedError" DOMException.
    if (m_attached_internals)
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals already attached"_string);

    // 6. If this's custom element state is not "precustomized" or "custom", then throw a "NotSupportedError" DOMException.
    if (!first_is_one_of(custom_element_state(), DOM::CustomElementState::Precustomized, DOM::CustomElementState::Custom))
        return WebIDL::NotSupportedError::create(realm(), "Custom element is in an invalid state to attach ElementInternals"_string);

    // 7. Set this's attached internals to a new ElementInternals instance whose target element is this.
    auto internals = ElementInternals::create(realm(), *this);

    m_attached_internals = internals;

    // 8. Return this's attached internals.
    return { internals };
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-popover
Optional<String> HTMLElement::popover() const
{
    // FIXME: This should probably be `Reflect` in the IDL.
    // The popover IDL attribute must reflect the popover attribute, limited to only known values.
    auto value = get_attribute(HTML::AttributeNames::popover);

    if (!value.has_value())
        return {};

    if (value.value().is_empty() || value.value().equals_ignoring_ascii_case("auto"sv))
        return "auto"_string;

    return "manual"_string;
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-popover
WebIDL::ExceptionOr<void> HTMLElement::set_popover(Optional<String> value)
{
    // FIXME: This should probably be `Reflect` in the IDL.
    // The popover IDL attribute must reflect the popover attribute, limited to only known values.
    if (value.has_value())
        return set_attribute(HTML::AttributeNames::popover, value.release_value());

    remove_attribute(HTML::AttributeNames::popover);
    return {};
}

void HTMLElement::adjust_computed_style(CSS::StyleProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (local_name() == HTML::TagNames::wbr) {
        if (style.display().is_contents())
            style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
    }
}

void HTMLElement::did_receive_focus()
{
    if (m_content_editable_state != ContentEditableState::True)
        return;

    auto editing_host = document().editing_host_manager();
    editing_host->set_active_contenteditable_element(this);

    DOM::Text* text = nullptr;
    for_each_in_inclusive_subtree_of_type<DOM::Text>([&](auto& node) {
        text = &node;
        return TraversalDecision::Continue;
    });

    if (!text) {
        editing_host->set_selection_anchor(*this, 0);
        return;
    }
    editing_host->set_selection_anchor(*text, text->length());
}

void HTMLElement::did_lose_focus()
{
    if (m_content_editable_state != ContentEditableState::True)
        return;

    document().editing_host_manager()->set_active_contenteditable_element(nullptr);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-accesskeylabel
String HTMLElement::access_key_label() const
{
    dbgln("FIXME: Implement HTMLElement::access_key_label()");
    return String {};
}

}
