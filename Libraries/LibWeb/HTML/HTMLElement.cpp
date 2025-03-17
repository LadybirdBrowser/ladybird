/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/LiveNodeList.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CloseWatcher.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/ElementInternals.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
#include <LibWeb/HTML/ToggleEvent.h>
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
    visitor.visit(m_popover_invoker);
    visitor.visit(m_popover_close_watcher);
}

// https://html.spec.whatwg.org/multipage/dom.html#block-rendering
void HTMLElement::block_rendering()
{
    // 1. Let document be el's node document.
    auto& document = this->document();

    // 2. If document allows adding render-blocking elements, then append el to document's render-blocking element set.
    if (document.allows_adding_render_blocking_elements()) {
        document.add_render_blocking_element(*this);
    }
}

// https://html.spec.whatwg.org/multipage/dom.html#unblock-rendering
void HTMLElement::unblock_rendering()
{
    // 1. Let document be el's node document.
    auto& document = this->document();

    // 2. Remove el from document's render-blocking element set.
    document.remove_render_blocking_element(*this);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#potentially-render-blocking
bool HTMLElement::is_potentially_render_blocking()
{
    // An element is potentially render-blocking if
    // FIXME: its blocking tokens set contains "render",
    // or if it is implicitly potentially render-blocking, which will be defined at the individual elements.
    return is_implicitly_potentially_render_blocking();
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

bool HTMLElement::is_focusable() const
{
    return is_editing_host() || get_attribute(HTML::AttributeNames::tabindex).has_value();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-iscontenteditable
bool HTMLElement::is_content_editable() const
{
    // The isContentEditable IDL attribute, on getting, must return true if the element is either an editing host or
    // editable, and false otherwise.
    return is_editable_or_editing_host();
}

StringView HTMLElement::content_editable() const
{
    switch (m_content_editable_state) {
    case ContentEditableState::True:
        return "true"sv;
    case ContentEditableState::False:
        return "false"sv;
    case ContentEditableState::PlaintextOnly:
        return "plaintext-only"sv;
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
    if (content_editable.equals_ignoring_ascii_case("plaintext-only"sv)) {
        MUST(set_attribute(HTML::AttributeNames::contenteditable, "plaintext-only"_string));
        return {};
    }
    if (content_editable.equals_ignoring_ascii_case("false"sv)) {
        MUST(set_attribute(HTML::AttributeNames::contenteditable, "false"_string));
        return {};
    }
    return WebIDL::SyntaxError::create(realm(), "Invalid contentEditable value, must be 'true', 'false', 'plaintext-only' or 'inherit'"_string);
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

            // 3. Append the result of creating an element given document, "br", and the HTML namespace to fragment.
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
        auto const* layout_text_node = as<Layout::TextNode>(layout_node);
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
    document().update_layout(DOM::UpdateLayoutReason::HTMLElementGetTheTextSteps);
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
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementOffsetParent);

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
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementOffsetTop);

    if (!paintable_box())
        return 0;

    CSSPixels top_border_edge_of_element = paintable_box()->absolute_united_border_box_rect().y();

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
    if (offset_parent->is_html_body_element() && !offset_parent->paintable_box()->is_positioned()) {
        top_padding_edge_of_offset_parent = 0;
    } else {
        top_padding_edge_of_offset_parent = offset_parent->paintable_box()->absolute_united_padding_box_rect().y();
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
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementOffsetLeft);

    if (!paintable_box())
        return 0;

    CSSPixels left_border_edge_of_element = paintable_box()->absolute_united_border_box_rect().x();

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
    if (offset_parent->is_html_body_element() && !offset_parent->paintable_box()->is_positioned()) {
        left_padding_edge_of_offset_parent = 0;
    } else {
        left_padding_edge_of_offset_parent = offset_parent->paintable_box()->absolute_united_padding_box_rect().x();
    }
    return (left_border_edge_of_element - left_padding_edge_of_offset_parent).to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-htmlelement-offsetwidth
int HTMLElement::offset_width() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementOffsetWidth);

    // 1. If the element does not have any associated box return zero and terminate this algorithm.
    auto const* box = paintable_box();
    if (!box)
        return 0;

    // 2. Return the unscaled width of the axis-aligned bounding box of the border boxes of all fragments generated by
    //    the element’s principal box, ignoring any transforms that apply to the element and its ancestors.
    //
    //    If the element’s principal box is an inline-level box which was "split" by a block-level descendant, also
    //    include fragments generated by the block-level descendants, unless they are zero width or height.
    return box->absolute_united_border_box_rect().width().to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-htmlelement-offsetheight
int HTMLElement::offset_height() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementOffsetHeight);

    // 1. If the element does not have any associated box return zero and terminate this algorithm.
    auto const* box = paintable_box();
    if (!box)
        return 0;

    // 2. Return the unscaled height of the axis-aligned bounding box of the border boxes of all fragments generated by
    //    the element’s principal box, ignoring any transforms that apply to the element and its ancestors.
    //
    //    If the element’s principal box is an inline-level box which was "split" by a block-level descendant, also
    //    include fragments generated by the block-level descendants, unless they are zero width or height.
    return box->absolute_united_border_box_rect().height().to_int();
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
            // No value maps to the "inherit" state.
            m_content_editable_state = ContentEditableState::Inherit;
        } else if (value->is_empty() || value->equals_ignoring_ascii_case("true"sv)) {
            // "true", an empty string or a missing value map to the "true" state.
            m_content_editable_state = ContentEditableState::True;
        } else if (value->equals_ignoring_ascii_case("false"sv)) {
            // "false" maps to the "false" state.
            m_content_editable_state = ContentEditableState::False;
        } else if (value->equals_ignoring_ascii_case("plaintext-only"sv)) {
            // "plaintext-only" maps to the "plaintext-only" state.
            m_content_editable_state = ContentEditableState::PlaintextOnly;
        } else {
            // Having an invalid value maps to the "inherit" state.
            m_content_editable_state = ContentEditableState::Inherit;
        }
    } else if (name == HTML::AttributeNames::inert) {
        // https://html.spec.whatwg.org/multipage/interaction.html#the-inert-attribute
        // The inert attribute is a boolean attribute that indicates, by its presence, that the element and all its flat tree descendants which don't otherwise escape inertness
        // (such as modal dialogs) are to be made inert by the user agent.
        auto is_inert = value.has_value();
        set_subtree_inertness(is_inert);
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

    [&]() {
        // https://html.spec.whatwg.org/multipage/popover.html#the-popover-attribute:concept-element-attributes-change-ext
        // https://whatpr.org/html/9457/popover.html#the-popover-attribute:concept-element-attributes-change-ext
        // The following attribute change steps, given element, localName, oldValue, value, and namespace, are used for all HTML elements:

        // 1. If namespace is not null, then return.
        if (namespace_.has_value())
            return;

        // 2. If localName is not popover, then return.
        if (name != HTML::AttributeNames::popover)
            return;

        // 3. If element's popover visibility state is in the showing state
        //    and oldValue and value are in different states,
        //    then run the hide popover algorithm given element, true, true, false, and true.
        if (m_popover_visibility_state == PopoverVisibilityState::Showing
            && popover_value_to_state(old_value) != popover_value_to_state(value))
            MUST(hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::Yes));
    }();
}

void HTMLElement::set_subtree_inertness(bool is_inert)
{
    set_inert(is_inert);
    for_each_in_subtree_of_type<HTMLElement>([&](auto& html_element) {
        if (html_element.has_attribute(HTML::AttributeNames::inert))
            return TraversalDecision::SkipChildrenAndContinue;
        // FIXME: Exclude elements that should escape inertness.
        html_element.set_inert(is_inert);
        return TraversalDecision::Continue;
    });
}

WebIDL::ExceptionOr<void> HTMLElement::cloned(Web::DOM::Node& copy, bool clone_children) const
{
    TRY(Base::cloned(copy, clone_children));
    TRY(HTMLOrSVGElement::cloned(copy, clone_children));
    return {};
}

void HTMLElement::inserted()
{
    Base::inserted();
    HTMLOrSVGElement::inserted();

    if (auto* parent_html_element = first_ancestor_of_type<HTMLElement>(); parent_html_element && parent_html_element->is_inert() && !has_attribute(HTML::AttributeNames::inert))
        set_subtree_inertness(true);
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
            return is<HTMLLabelElement>(node) && as<HTMLLabelElement>(node).control() == this;
        });
    }

    return m_labels;
}

Variant<bool, double, String> HTMLElement::hidden() const
{
    // 1. If the hidden attribute is in the hidden until found state, then return "until-found".
    if (get_attribute(HTML::AttributeNames::hidden) == "until-found")
        return "until-found"_string;
    // 2. If the hidden attribute is set, then return true.
    if (has_attribute(HTML::AttributeNames::hidden))
        return true;
    // 3. Return false.
    return false;
}

void HTMLElement::set_hidden(Variant<bool, double, String> const& given_value)
{
    // 1. If the given value is a string that is an ASCII case-insensitive match for "until-found", then set the hidden attribute to "until-found".
    if (given_value.has<String>()) {
        auto const& string = given_value.get<String>();
        if (string.equals_ignoring_ascii_case("until-found"sv)) {
            MUST(set_attribute(HTML::AttributeNames::hidden, "until-found"_string));
            return;
        }
        // 3. Otherwise, if the given value is the empty string, then remove the hidden attribute.
        if (string.is_empty()) {
            remove_attribute(HTML::AttributeNames::hidden);
            return;
        }
        // 4. Otherwise, if the given value is null, then remove the hidden attribute.
        if (string.equals_ignoring_ascii_case("null"sv) || string.equals_ignoring_ascii_case("undefined"sv)) {
            remove_attribute(HTML::AttributeNames::hidden);
            return;
        }
    }
    // 2. Otherwise, if the given value is false, then remove the hidden attribute.
    else if (given_value.has<bool>()) {
        if (!given_value.get<bool>()) {
            remove_attribute(HTML::AttributeNames::hidden);
            return;
        }
    }
    // 5. Otherwise, if the given value is 0, then remove the hidden attribute.
    // 6. Otherwise, if the given value is NaN, then remove the hidden attribute.
    else if (given_value.has<double>()) {
        auto const& double_value = given_value.get<double>();
        if (double_value == 0 || isnan(double_value)) {
            remove_attribute(HTML::AttributeNames::hidden);
            return;
        }
    }
    // 7. Otherwise, set the hidden attribute to the empty string.
    MUST(set_attribute(HTML::AttributeNames::hidden, ""_string));
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
    if (local_name() == TagNames::aside) {
        // https://w3c.github.io/html-aam/#el-aside
        for (auto const* ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
            if (ancestor->local_name().is_one_of(TagNames::article, TagNames::aside, TagNames::nav, TagNames::section)
                && accessible_name(document()).value().is_empty())
                return ARIA::Role::generic;
        }
        // https://w3c.github.io/html-aam/#el-aside-ancestorbodymain
        return ARIA::Role::complementary;
    }
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
    // https://w3c.github.io/html-aam/#el-dd
    if (local_name() == TagNames::dd)
        return ARIA::Role::definition;
    // https://wpt.fyi/results/html-aam/dir-role.tentative.html
    if (local_name() == TagNames::dir)
        return ARIA::Role::list;
    // https://w3c.github.io/html-aam/#el-dt
    if (local_name() == TagNames::dt)
        return ARIA::Role::term;
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
    // https://www.w3.org/TR/html-aria/#el-header
    if (local_name() == TagNames::footer || local_name() == TagNames::header) {
        // If not a descendant of an article, aside, main, nav or section element, or an element with role=article,
        // complementary, main, navigation or region then (footer) role=contentinfo (header) role=banner. Otherwise,
        // role=generic.
        for (auto const* ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
            if (ancestor->local_name().is_one_of(TagNames::article, TagNames::aside, TagNames::main, TagNames::nav, TagNames::section)) {
                if (local_name() == TagNames::footer)
                    return ARIA::Role::sectionfooter;
                return ARIA::Role::sectionheader;
            }
            if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::article, ARIA::Role::complementary, ARIA::Role::main, ARIA::Role::navigation, ARIA::Role::region)) {
                if (local_name() == TagNames::footer)
                    return ARIA::Role::sectionfooter;
                return ARIA::Role::sectionheader;
            }
        }
        // then (footer) role=contentinfo.
        if (local_name() == TagNames::footer)
            return ARIA::Role::contentinfo;
        // (header) role=banner
        return ARIA::Role::banner;
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
    // https://www.w3.org/TR/html-aria/#el-mark
    if (local_name() == TagNames::mark)
        return ARIA::Role::mark;
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
        // role=region if the section element has an accessible name
        if (!accessible_name(document()).value().is_empty())
            return ARIA::Role::region;
        // Otherwise, role=generic
        return ARIA::Role::generic;
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
String HTMLElement::get_an_elements_target(Optional<String> target) const
{
    // To get an element's target, given an a, area, or form element element, and an optional string-or-null target (default null), run these steps:

    // 1. If target is null, then:
    if (!target.has_value()) {
        // 1. If element has a target attribute, then set target to that attribute's value.
        if (auto maybe_target = attribute(AttributeNames::target); maybe_target.has_value()) {
            target = maybe_target.release_value();
        }
        // 2. Otherwise, if element's node document contains a base element with a target attribute,
        //    set target to the value of the target attribute of the first such base element.
        if (auto base_element = document().first_base_element_with_target_in_tree_order())
            target = base_element->attribute(AttributeNames::target);
    }

    // 2. If target is not null, and contains an ASCII tab or newline and a U+003C (<), then set target to "_blank".
    if (target.has_value() && target->bytes_as_string_view().contains("\t\n\r"sv) && target->contains('<'))
        target = "_blank"_string;

    // 3. Return target.
    return target.value_or({});
}

// https://html.spec.whatwg.org/multipage/links.html#get-an-element's-noopener
TokenizedFeature::NoOpener HTMLElement::get_an_elements_noopener(URL::URL const& url, StringView target) const
{
    // To get an element's noopener, given an a, area, or form element element, a URL record url, and a string target,
    // perform the following steps. They return a boolean.
    auto rel = MUST(get_attribute_value(HTML::AttributeNames::rel).to_lowercase());
    auto link_types = rel.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

    // 1. If element's link types include the noopener or noreferrer keyword, then return true.
    if (link_types.contains_slow("noopener"sv) || link_types.contains_slow("noreferrer"sv))
        return TokenizedFeature::NoOpener::Yes;

    // 2. If element's link types do not include the opener keyword and
    //    target is an ASCII case-insensitive match for "_blank", then return true.
    if (!link_types.contains_slow("opener"sv) && Infra::is_ascii_case_insensitive_match(target, "_blank"sv))
        return TokenizedFeature::NoOpener::Yes;

    // 3. If url's blob URL entry is not null:
    if (url.blob_url_entry().has_value()) {
        // 1. Let blobOrigin be url's blob URL entry's environment's origin.
        auto const& blob_origin = url.blob_url_entry()->environment.origin;

        // 2. Let topLevelOrigin be element's relevant settings object's top-level origin.
        auto const& top_level_origin = relevant_settings_object(*this).top_level_origin;

        // 3. If blobOrigin is not same site with topLevelOrigin, then return true.
        if (!blob_origin.is_same_site(top_level_origin))
            return TokenizedFeature::NoOpener::Yes;
    }

    // 4. Return false.
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

Optional<String> HTMLElement::popover_value_to_state(Optional<String> value)
{
    if (!value.has_value())
        return {};

    if (value.value().is_empty() || value.value().equals_ignoring_ascii_case("auto"sv))
        return "auto"_string;

    if (value.value().equals_ignoring_ascii_case("hint"sv))
        return "hint"_string;

    return "manual"_string;
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-popover
Optional<String> HTMLElement::popover() const
{
    // FIXME: This should probably be `Reflect` in the IDL.
    // The popover IDL attribute must reflect the popover attribute, limited to only known values.
    auto value = get_attribute(HTML::AttributeNames::popover);
    return popover_value_to_state(value);
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

void HTMLElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (local_name() == HTML::TagNames::wbr) {
        if (style.display().is_contents())
            style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
    }
}

// https://html.spec.whatwg.org/multipage/popover.html#check-popover-validity
// https://whatpr.org/html/9457/popover.html#check-popover-validity
WebIDL::ExceptionOr<bool> HTMLElement::check_popover_validity(ExpectedToBeShowing expected_to_be_showing, ThrowExceptions throw_exceptions, GC::Ptr<DOM::Document> expected_document, IgnoreDomState ignore_dom_state)
{
    // 1. If ignoreDomState is false and element's popover attribute is in the no popover state, then:
    if (ignore_dom_state == IgnoreDomState::No && !popover().has_value()) {
        // 1.1. If throwExceptions is true, then throw a "NotSupportedError" DOMException.
        if (throw_exceptions == ThrowExceptions::Yes)
            return WebIDL::NotSupportedError::create(realm(), "Element is not a popover"_string);
        // 1.2. Return false.
        return false;
    }

    // 2. If any of the following are true:
    // - expectedToBeShowing is true and element's popover visibility state is not showing; or
    // - expectedToBeShowing is false and element's popover visibility state is not hidden,
    if ((expected_to_be_showing == ExpectedToBeShowing::Yes && m_popover_visibility_state != PopoverVisibilityState::Showing) || (expected_to_be_showing == ExpectedToBeShowing::No && m_popover_visibility_state != PopoverVisibilityState::Hidden)) {
        // then return false.
        return false;
    }

    // 3. If any of the following are true:
    // - ignoreDomState is false and element is not connected;
    // - element's node document is not fully active;
    // - ignoreDomState is false and expectedDocument is not null and element's node document is not expectedDocument;
    // - element is a dialog element and its is modal flage is set to true; or
    // - FIXME: element's fullscreen flag is set,
    // then:
    // 3.1 If throwExceptions is true, then throw an "InvalidStateError" DOMException.
    // 3.2 Return false.

    if ((ignore_dom_state == IgnoreDomState::No && !is_connected())
        || !document().is_fully_active()
        || (ignore_dom_state == IgnoreDomState::No && expected_document && &document() != expected_document)
        || (is<HTMLDialogElement>(*this) && as<HTMLDialogElement>(*this).is_modal())) {
        if (throw_exceptions == ThrowExceptions::Yes)
            return WebIDL::InvalidStateError::create(realm(), "Element is not in a valid state to show a popover"_string);
        return false;
    }

    // 4. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-showpopover
WebIDL::ExceptionOr<void> HTMLElement::show_popover_for_bindings(ShowPopoverOptions const& options)
{
    // 1. Let invoker be options["source"] if it exists; otherwise, null.
    auto invoker = options.source;
    // 2. Run show popover given this, true, and invoker.
    return show_popover(ThrowExceptions::Yes, invoker);
}

// https://html.spec.whatwg.org/multipage/popover.html#show-popover
// https://whatpr.org/html/9457/popover.html#show-popover
WebIDL::ExceptionOr<void> HTMLElement::show_popover(ThrowExceptions throw_exceptions, GC::Ptr<HTMLElement> invoker)
{
    // 1. If the result of running check popover validity given element, false, throwExceptions, null and false is false, then return.
    if (!TRY(check_popover_validity(ExpectedToBeShowing::No, throw_exceptions, nullptr, IgnoreDomState::No)))
        return {};

    // 2. Let document be element's node document.
    auto& document = this->document();

    // 3. Assert: element's popover invoker is null.
    VERIFY(!m_popover_invoker);

    // 4. Assert: element is not in document's top layer.
    VERIFY(!in_top_layer());

    // 5. Let nestedShow be element's popover showing or hiding.
    auto nested_show = m_popover_showing_or_hiding;

    // 6. Let fireEvents be the boolean negation of nestedShow.
    FireEvents fire_events = nested_show ? FireEvents::No : FireEvents::Yes;

    // 7. Set element's popover showing or hiding to true.
    m_popover_showing_or_hiding = true;

    // 8. Let cleanupShowingFlag be the following steps:
    auto cleanup_showing_flag = [&nested_show, this] {
        // 8.1. If nestedShow is false, then set element's popover showing or hiding to false.
        if (!nested_show)
            m_popover_showing_or_hiding = false;
    };

    // 9. If the result of firing an event named beforetoggle, using ToggleEvent, with the cancelable attribute initialized to true, the oldState attribute initialized to "closed", and the newState attribute initialized to "open" at element is false, then run cleanupShowingFlag and return.
    ToggleEventInit event_init {};
    event_init.old_state = "closed"_string;
    event_init.new_state = "open"_string;
    event_init.cancelable = true;
    if (!dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::beforetoggle, move(event_init)))) {
        cleanup_showing_flag();
        return {};
    }

    // 10. If the result of running check popover validity given element, false, throwExceptions, document, and false is false, then run cleanupShowingFlag and return.
    if (!TRY(check_popover_validity(ExpectedToBeShowing::No, throw_exceptions, document, IgnoreDomState::No))) {
        cleanup_showing_flag();
        return {};
    }

    // 11. Let shouldRestoreFocus be false.
    auto should_restore_focus = FocusPreviousElement::No;

    // 12. Let originalType be the current state of element's popover attribute.
    auto original_type = popover();

    // 13. Let stackToAppendTo be null.
    enum class StackToAppendTo : u8 {
        Null,
        Auto,
        Hint,
    };
    StackToAppendTo stack_to_append_to = StackToAppendTo::Null;

    // 16. If originalType is the auto state, then:
    if (original_type == "auto"sv) {
        // 1. Run close entire popover list given document's showing hint popover list, shouldRestoreFocus, and fireEvents.
        close_entire_popover_list(document.showing_hint_popover_list(), should_restore_focus, fire_events);

        // 2. Let ancestor be the result of running the topmost popover ancestor algorithm given element, document's showing auto popover list, invoker, and true.
        Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> ancestor = topmost_popover_ancestor(this, document.showing_auto_popover_list(), invoker, IsPopover::Yes);

        // 3. If ancestor is null, then set ancestor to document.
        if (!ancestor.get<GC::Ptr<HTMLElement>>())
            ancestor = GC::Ptr(document);

        // 4. Run hide all popovers until given ancestor, shouldRestoreFocus, and fireEvents.
        hide_all_popovers_until(ancestor, should_restore_focus, fire_events);

        // 5. Set stackToAppendTo to "auto".
        stack_to_append_to = StackToAppendTo::Auto;
    }

    // 17. If originalType is the hint state, then:
    if (original_type == "hint"sv) {

        // AD-HOC: Steps 14 and 15 have been moved here to avoid hitting the `popover != manual` assertion in the topmost popover ancestor algorithm.
        // Spec issue: https://github.com/whatwg/html/issues/10988.
        // 14. Let autoAncestor be the result of running the topmost popover ancestor algorithm given element, document's showing auto popover list, invoker, and true.
        auto auto_ancestor = topmost_popover_ancestor(this, document.showing_auto_popover_list(), invoker, IsPopover::Yes);

        // 15. Let hintAncestor be the result of running the topmost popover ancestor algorithm given element, document's showing hint popover list, invoker, and true.
        auto hint_ancestor = topmost_popover_ancestor(this, document.showing_hint_popover_list(), invoker, IsPopover::Yes);

        // 1. If hintAncestor is not null, then:
        if (hint_ancestor) {
            // 1. Run hide all popovers until given hintAncestor, shouldRestoreFocus, and fireEvents.
            hide_all_popovers_until(hint_ancestor, should_restore_focus, fire_events);

            // 2. Set stackToAppendTo to "hint".
            stack_to_append_to = StackToAppendTo::Hint;
        }
        // 2. Otherwise:
        else {
            // 1. Run close entire popover list given document's showing hint popover list, shouldRestoreFocus, and fireEvents.
            close_entire_popover_list(document.showing_hint_popover_list(), should_restore_focus, fire_events);

            // 2. If autoAncestor is not null, then:
            if (auto_ancestor) {
                // 1. Run hide all popovers until given autoAncestor, shouldRestoreFocus, and fireEvents.
                hide_all_popovers_until(auto_ancestor, should_restore_focus, fire_events);

                // 2. Set stackToAppendTo to "auto".
                stack_to_append_to = StackToAppendTo::Auto;
            }
            // 3. Otherwise, set stackToAppendTo to "hint".
            else {
                stack_to_append_to = StackToAppendTo::Hint;
            }
        }
    }

    // 18. If originalType is auto or hint, then:
    if (original_type.has_value() && original_type.value().is_one_of("auto", "hint")) {
        // 1. Assert: stackToAppendTo is not null.
        VERIFY(stack_to_append_to != StackToAppendTo::Null);

        // 2. If originalType is not equal to the value of element's popover attribute, then:
        if (original_type != popover()) {
            // 1. If throwExceptions is true, then throw a "InvalidStateError" DOMException.
            if (throw_exceptions == ThrowExceptions::Yes)
                return WebIDL::InvalidStateError::create(realm(), "Element is not in a valid state to show a popover"_string);

            // 2. Return.
            return {};
        }

        // 3. If the result of running check popover validity given element, false, throwExceptions, document, and false is false, then run cleanupShowingFlag and return.
        if (!TRY(check_popover_validity(ExpectedToBeShowing::No, throw_exceptions, document, IgnoreDomState::No))) {
            cleanup_showing_flag();
            return {};
        }

        // FIXME: 4. If the result of running topmost auto or hint popover on document is null, then set shouldRestoreFocus to true.

        // 5. If stackToAppendTo is "auto":
        if (stack_to_append_to == StackToAppendTo::Auto) {
            // 1. Assert: document's showing auto popover list does not contain element.
            VERIFY(!document.showing_auto_popover_list().contains_slow(GC::Ref(*this)));

            // AD-HOC: Append element to the document's showing auto popover list.
            // Spec issue: https://github.com/whatwg/html/issues/11007
            document.showing_auto_popover_list().append(*this);

            // 2. Set element's opened in popover mode to "auto".
            m_opened_in_popover_mode = "auto"_string;
        }
        // Otherwise:
        else {
            // 1. Assert: stackToAppendTo is "hint".
            VERIFY(stack_to_append_to == StackToAppendTo::Hint);

            // 2. Assert: document's showing hint popover list does not contain element.
            VERIFY(!document.showing_hint_popover_list().contains_slow(GC::Ref(*this)));

            // AD-HOC: Append element to the document's showing hint popover list.
            // Spec issue: https://github.com/whatwg/html/issues/11007
            document.showing_hint_popover_list().append(*this);

            // 3. Set element's opened in popover mode to "hint".
            m_opened_in_popover_mode = "hint"_string;
        }

        // 6. Set element's popover close watcher to the result of establishing a close watcher given element's relevant global object, with:
        m_popover_close_watcher = CloseWatcher::establish(*document.window());
        // - cancelAction being to return true.
        // We simply don't add an event listener for the cancel action.
        // - closeAction being to hide a popover given element, true, true, and false.
        auto close_callback_function = JS::NativeFunction::create(
            realm(), [this](JS::VM&) {
                MUST(hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::No));

                return JS::js_undefined();
            },
            0, FlyString {}, &realm());
        auto close_callback = realm().heap().allocate<WebIDL::CallbackType>(*close_callback_function, realm());
        m_popover_close_watcher->add_event_listener_without_options(HTML::EventNames::close, DOM::IDLEventListener::create(realm(), close_callback));
        // - getEnabledState being to return true.
        m_popover_close_watcher->set_enabled(true);
    }
    // FIXME: 19. Set element's previously focused element to null.
    // FIXME: 20. Let originallyFocusedElement be document's focused area of the document's DOM anchor.
    // 21. Add an element to the top layer given element.
    document.add_an_element_to_the_top_layer(*this);
    // 22. Set element's popover visibility state to showing.
    m_popover_visibility_state = PopoverVisibilityState::Showing;
    // 23. Set element's popover invoker to invoker.
    m_popover_invoker = invoker;
    // FIXME: 24. Set element's implicit anchor element to invoker.
    // FIXME: 25. Run the popover focusing steps given element.
    // FIXME: 26. If shouldRestoreFocus is true and element's popover attribute is not in the no popover state, then set element's previously focused element to originallyFocusedElement.
    // 27. Queue a popover toggle event task given element, "closed", and "open".
    queue_a_popover_toggle_event_task("closed"_string, "open"_string);
    // 28. Run cleanupShowingFlag.
    cleanup_showing_flag();

    return {};
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-hidepopover
// https://whatpr.org/html/9457/popover.html#dom-hidepopover
WebIDL::ExceptionOr<void> HTMLElement::hide_popover_for_bindings()
{
    // The hidePopover() method steps are to run the hide popover algorithm given this, true, true, true, and false.
    return hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::Yes, IgnoreDomState::No);
}

// https://html.spec.whatwg.org/multipage/popover.html#hide-popover-algorithm
// https://whatpr.org/html/9457/popover.html#hide-popover-algorithm
WebIDL::ExceptionOr<void> HTMLElement::hide_popover(FocusPreviousElement focus_previous_element, FireEvents fire_events, ThrowExceptions throw_exceptions, IgnoreDomState ignore_dom_state)
{
    // 1. If the result of running check popover validity given element, true, throwExceptions, null and ignoreDomState is false, then return.
    if (!TRY(check_popover_validity(ExpectedToBeShowing::Yes, throw_exceptions, nullptr, ignore_dom_state)))
        return {};

    // 2. Let document be element's node document.
    auto& document = this->document();

    // 3. Let nestedHide be element's popover showing or hiding.
    auto nested_hide = m_popover_showing_or_hiding;

    // 4. Set element's popover showing or hiding to true.
    m_popover_showing_or_hiding = true;

    // 5. If nestedHide is true, then set fireEvents to false.
    if (nested_hide)
        fire_events = FireEvents::No;

    // 6. Let cleanupSteps be the following steps:
    auto cleanup_steps = [&nested_hide, this] {
        // 6.1. If nestedHide is false, then set element's popover showing or hiding to false.
        if (nested_hide)
            m_popover_showing_or_hiding = false;
        // 6.2. If element's popover close watcher is not null, then:
        if (m_popover_close_watcher) {
            // 6.2.1. Destroy element's popover close watcher.
            m_popover_close_watcher->destroy();
            // 6.2.2. Set element's popover close watcher to null.
            m_popover_close_watcher = nullptr;
        }
    };

    // AD-HOC: This implementation checks "opened in popover mode" instead of the current popover state.
    // Spec issue: https://github.com/whatwg/html/issues/10996.
    // 7. If element's popover attribute is in the auto state or the hint state, then:
    if (m_opened_in_popover_mode.has_value() && m_opened_in_popover_mode.value().is_one_of("auto", "hint")) {
        // 7.1. Run hide all popovers until given element, focusPreviousElement, and fireEvents.
        hide_all_popovers_until(GC::Ptr(this), focus_previous_element, fire_events);

        // 7.2. If the result of running check popover validity given element, true, throwExceptions, and ignoreDomState is false, then run cleanupSteps and return.
        if (!TRY(check_popover_validity(ExpectedToBeShowing::Yes, throw_exceptions, nullptr, ignore_dom_state))) {
            cleanup_steps();
            return {};
        }
    }
    // 8. Let autoPopoverListContainsElement be true if document's showing auto popover list's last item is element, otherwise false.
    auto const& showing_popovers = document.showing_auto_popover_list();
    bool auto_popover_list_contains_element = !showing_popovers.is_empty() && showing_popovers.last() == this;

    // 9. Set element's popover invoker to null.
    m_popover_invoker = nullptr;

    // 10. If fireEvents is true:
    if (fire_events == FireEvents::Yes) {
        // 10.1. Fire an event named beforetoggle, using ToggleEvent, with the oldState attribute initialized to "open" and the newState attribute initialized to "closed" at element.
        ToggleEventInit event_init {};
        event_init.old_state = "open"_string;
        event_init.new_state = "closed"_string;
        dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::beforetoggle, move(event_init)));

        // 10.2. If autoPopoverListContainsElement is true and document's showing auto popover list's last item is not element, then run hide all popovers until given element, focusPreviousElement, and false.
        if (auto_popover_list_contains_element && (showing_popovers.is_empty() || showing_popovers.last() != this))
            hide_all_popovers_until(GC::Ptr(this), focus_previous_element, FireEvents::No);

        // 10.3. If the result of running check popover validity given element, true, throwExceptions, null, and ignoreDomState is false, then run cleanupSteps and return.
        if (!TRY(check_popover_validity(ExpectedToBeShowing::Yes, throw_exceptions, nullptr, ignore_dom_state))) {
            cleanup_steps();
            return {};
        }
        // 10.4. Request an element to be removed from the top layer given element.
        document.request_an_element_to_be_remove_from_the_top_layer(*this);
    } else {
        // 11. Otherwise, remove an element from the top layer immediately given element.
        document.remove_an_element_from_the_top_layer_immediately(*this);
    }

    // AD-HOC: The following block of code is all ad-hoc.
    // Spec issue: https://github.com/whatwg/html/issues/11007

    // If element's opened in popover mode is "auto" or "hint":
    if (m_opened_in_popover_mode.has_value() && m_opened_in_popover_mode.value().is_one_of("auto", "hint")) {
        // If document's showing hint popover list's last item is element:
        auto& hint_popovers = document.showing_hint_popover_list();
        if (!hint_popovers.is_empty() && hint_popovers.last() == this) {
            // Assert: element's opened in popover mode is "hint".
            VERIFY(m_opened_in_popover_mode == "hint"sv);

            // Remove the last item from document's showing hint popover list.
            hint_popovers.remove(hint_popovers.size() - 1);
        }
        // Otherwise:
        else {
            // Assert: document's showing auto popover list's last item is element.
            auto& auto_popovers = document.showing_auto_popover_list();
            VERIFY(!auto_popovers.is_empty() && auto_popovers.last() == this);

            // Remove the last item from document's showing auto popover list.
            auto_popovers.remove(auto_popovers.size() - 1);
        }
    }

    // 12. Set element's opened in popover mode to null.
    m_opened_in_popover_mode = {};

    // 13. Set element's popover visibility state to hidden.
    m_popover_visibility_state = PopoverVisibilityState::Hidden;

    // 14. If fireEvents is true, then queue a popover toggle event task given element, "open", and "closed".
    if (fire_events == FireEvents::Yes)
        queue_a_popover_toggle_event_task("open"_string, "closed"_string);

    // FIXME: 15. Let previouslyFocusedElement be element's previously focused element.

    // FIXME: 16. If previouslyFocusedElement is not null, then:
    // FIXME: 16.1. Set element's previously focused element to null.
    // FIXME: 16.2. If focusPreviousElement is true and document's focused area of the document's DOM anchor is a shadow-including inclusive descendant of element, then run the focusing steps for previouslyFocusedElement; the viewport should not be scrolled by doing this step.

    // 17. Run cleanupSteps.
    cleanup_steps();

    return {};
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-togglepopover
// https://whatpr.org/html/9457/popover.html#dom-togglepopover
WebIDL::ExceptionOr<bool> HTMLElement::toggle_popover(TogglePopoverOptionsOrForceBoolean const& options)
{
    // 1. Let force be null.
    Optional<bool> force;
    GC::Ptr<HTMLElement> invoker;

    // 2. If options is a boolean, set force to options.
    options.visit(
        [&force](bool forceBool) {
            force = forceBool;
        },
        [&force, &invoker](TogglePopoverOptions options) {
            // 3. Otherwise, if options["force"] exists, set force to options["force"].
            force = options.force;
            // 4. Let invoker be options["source"] if it exists; otherwise, null.
            invoker = options.source;
        });

    // 5. If this's popover visibility state is showing, and force is null or false, then run the hide popover algorithm given this, true, true, true, and false.
    if (popover_visibility_state() == PopoverVisibilityState::Showing && (!force.has_value() || !force.value()))
        TRY(hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::Yes, IgnoreDomState::No));
    // 6. Otherwise, if force is not present or true, then run show popover given this true, and invoker.
    else if (!force.has_value() || force.value())
        TRY(show_popover(ThrowExceptions::Yes, invoker));
    // 7. Otherwise:
    else {
        // 7.1 Let expectedToBeShowing be true if this's popover visibility state is showing; otherwise false.
        ExpectedToBeShowing expected_to_be_showing = popover_visibility_state() == PopoverVisibilityState::Showing ? ExpectedToBeShowing::Yes : ExpectedToBeShowing::No;
        // 7.2 Run check popover validity given expectedToBeShowing, true, null, and false.
        TRY(check_popover_validity(expected_to_be_showing, ThrowExceptions::Yes, nullptr, IgnoreDomState::No));
    }
    // 8. Return true if this's popover visibility state is showing; otherwise false.
    return popover_visibility_state() == PopoverVisibilityState::Showing;
}

// AD-HOC: This implementation checks "opened in popover mode" instead of the current popover state.
// Spec issue: https://github.com/whatwg/html/issues/10996.
// https://html.spec.whatwg.org/multipage/popover.html#hide-all-popovers-until
void HTMLElement::hide_all_popovers_until(Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> endpoint, FocusPreviousElement focus_previous_element, FireEvents fire_events)
{
    // To hide all popovers until, given an HTML element or Document endpoint, a boolean focusPreviousElement, and a boolean fireEvents:

    // 1. If endpoint is an HTML element and endpoint is not in the popover showing state, then return.
    if (endpoint.has<GC::Ptr<HTMLElement>>() && endpoint.get<GC::Ptr<HTMLElement>>()->popover_visibility_state() != PopoverVisibilityState::Showing)
        return;

    // 2. Let document be endpoint's node document.
    auto const* document = endpoint.visit([](auto endpoint) { return &endpoint->document(); });

    // 3. Assert: endpoint is a Document or endpoint's popover visibility state is showing.
    VERIFY(endpoint.has<GC::Ptr<DOM::Document>>() || endpoint.get<GC::Ptr<HTMLElement>>()->popover_visibility_state() == PopoverVisibilityState::Showing);

    // 4. Assert: endpoint is a Document or endpoint's popover attribute is in the auto state or endpoint's popover attribute is in the hint state.
    VERIFY(endpoint.has<GC::Ptr<DOM::Document>>() || endpoint.get<GC::Ptr<HTMLElement>>()->m_opened_in_popover_mode->is_one_of("auto", "hint"));

    // 5. If endpoint is a Document:
    if (endpoint.has<GC::Ptr<DOM::Document>>()) {
        // 1. Run close entire popover list given document's showing hint popover list, focusPreviousElement, and fireEvents.
        close_entire_popover_list(document->showing_hint_popover_list(), focus_previous_element, fire_events);

        // 2. Run close entire popover list given document's showing auto popover list, focusPreviousElement, and fireEvents.
        close_entire_popover_list(document->showing_auto_popover_list(), focus_previous_element, fire_events);

        // 3. Return.
        return;
    }

    // 6. If document's showing hint popover list contains endpoint:
    auto endpoint_element = endpoint.get<GC::Ptr<HTMLElement>>();
    if (document->showing_hint_popover_list().contains_slow(GC::Ref(*endpoint_element))) {
        // 1. Assert: endpoint's popover attribute is in the hint state.
        VERIFY(endpoint_element->m_opened_in_popover_mode == "hint"sv);

        // 2. Run hide popover stack until given endpoint, document's showing hint popover list, focusPreviousElement, and fireEvents.
        endpoint_element->hide_popover_stack_until(document->showing_hint_popover_list(), focus_previous_element, fire_events);

        // 3. Return.
        return;
    }

    // 7. Run close entire popover list given document's showing hint popover list, focusPreviousElement, and fireEvents.
    close_entire_popover_list(document->showing_hint_popover_list(), focus_previous_element, fire_events);

    // 8. If document's showing auto popover list does not contain endpoint, then return.
    if (!document->showing_auto_popover_list().contains_slow(GC::Ref(*endpoint_element)))
        return;

    // 9. Run hide popover stack until given endpoint, document's showing auto popover list, focusPreviousElement, and fireEvents.
    endpoint_element->hide_popover_stack_until(document->showing_auto_popover_list(), focus_previous_element, fire_events);
}

// https://html.spec.whatwg.org/multipage/popover.html#hide-popover-stack-until
void HTMLElement::hide_popover_stack_until(Vector<GC::Ref<HTMLElement>> const& popover_list, FocusPreviousElement focus_previous_element, FireEvents fire_events)
{
    // To hide popover stack until, given an HTML element endpoint, a list popoverList, a boolean focusPreviousElement, and a boolean fireEvents:

    // 1. Let repeatingHide be false.
    bool repeating_hide = false;

    // 2. Perform the following steps at least once:
    do {
        // 1. Let lastToHide be null.
        GC::Ptr<HTMLElement> last_to_hide;

        // 2. For each popover in popoverList:
        // AD-HOC: This needs to be iterated in reverse because step 4 hides items in reverse.
        for (auto const& popover : popover_list.in_reverse()) {
            // 1. If popover is endpoint, then break.
            if (popover == this)
                break;

            // 2. Set lastToHide to popover.
            last_to_hide = popover;
        }

        // 3. If lastToHide is null, then return.
        if (!last_to_hide)
            return;

        // 4. While lastToHide's popover visibility state is showing:
        while (last_to_hide->popover_visibility_state() == PopoverVisibilityState::Showing) {
            // 1. Assert: popoverList is not empty.
            VERIFY(!popover_list.is_empty());

            // 2. Run the hide popover algorithm given the last item in popoverList, focusPreviousElement, fireEvents, and false.
            MUST(popover_list.last()->hide_popover(focus_previous_element, fire_events, ThrowExceptions::No, IgnoreDomState::No));
        }

        // 5. Assert: repeatingHide is false or popoverList's last item is endpoint.
        VERIFY(!repeating_hide || popover_list.last() == this);

        // 6. Set repeatingHide to true if popoverList contains endpoint and popoverList's last item is not endpoint, otherwise false.
        repeating_hide = popover_list.contains_slow(GC::Ref(*this)) && popover_list.last() != this;

        // 7. If repeatingHide is true, then set fireEvents to false.
        if (repeating_hide)
            fire_events = FireEvents::No;

    } while (repeating_hide);
    // and keep performing them while repeatingHide is true.
}

// https://html.spec.whatwg.org/multipage/popover.html#close-entire-popover-list
void HTMLElement::close_entire_popover_list(Vector<GC::Ref<HTMLElement>> const& popover_list, FocusPreviousElement focus_previous_element, FireEvents fire_events)
{
    // To close entire popover list given a list popoverList, a boolean focusPreviousElement, and a boolean fireEvents:

    // FIXME: If an event handler opens a new popover then this could be an infinite loop.
    // 1. While popoverList is not empty:
    while (!popover_list.is_empty()) {
        // 1. Run the hide popover algorithm given popoverList's last item, focusPreviousElement, fireEvents, and false.
        MUST(popover_list.last()->hide_popover(focus_previous_element, fire_events, ThrowExceptions::No, IgnoreDomState::No));
    }
}

// https://html.spec.whatwg.org/multipage/popover.html#topmost-popover-ancestor
GC::Ptr<HTMLElement> HTMLElement::topmost_popover_ancestor(GC::Ptr<DOM::Node> new_popover_or_top_layer_element, Vector<GC::Ref<HTMLElement>> const& popover_list, GC::Ptr<HTMLElement> invoker, IsPopover is_popover)
{
    // To find the topmost popover ancestor, given a Node newPopoverOrTopLayerElement, a list popoverList, an HTML element or null invoker, and a boolean isPopover, perform the following steps. They return an HTML element or null.

    // 1. If isPopover is true:
    auto* new_popover = as_if<HTML::HTMLElement>(*new_popover_or_top_layer_element);
    if (is_popover == IsPopover::Yes) {
        // 1. Assert: newPopoverOrTopLayerElement is an HTML element.
        VERIFY(new_popover);

        // 2. Assert: newPopoverOrTopLayerElement's popover attribute is not in the no popover state or the manual state.
        VERIFY(!new_popover->popover().has_value() || new_popover->popover().value() != "manual"sv);

        // 3. Assert: newPopoverOrTopLayerElement's popover visibility state is not in the popover showing state.
        VERIFY(new_popover->popover_visibility_state() != PopoverVisibilityState::Showing);
    }
    // 2. Otherwise:
    else {
        // 1. Assert: invoker is null.
        VERIFY(!invoker);
    }

    // 3. Let popoverPositions be an empty ordered map.
    OrderedHashMap<GC::Ref<HTMLElement>, int> popover_positions;

    // 4. Let index be 0.
    int index = 0;

    // 5. For each popover of popoverList:
    for (auto const& popover : popover_list) {
        // 1. Set popoverPositions[popover] to index.
        popover_positions.set(*popover, index);

        // 2. Increment index by 1.
        index++;
    }

    // 6. If isPopover is true, then set popoverPositions[newPopoverOrTopLayerElement] to index.
    if (is_popover == IsPopover::Yes)
        popover_positions.set(*new_popover, index);

    // 7. Increment index by 1.
    index++;

    // 8. Let topmostPopoverAncestor be null.
    GC::Ptr<HTMLElement> topmost_popover_ancestor;

    // 9. Let checkAncestor be an algorithm which performs the following steps given candidate:
    auto check_ancestor = [&](auto candidate) {
        // 1. If candidate is null, then return.
        if (!candidate)
            return;

        // 2. Let okNesting be false.
        bool ok_nesting = false;

        // 3. Let candidateAncestor be null.
        GC::Ptr<HTMLElement> candidate_ancestor;

        // 4. While okNesting is false:
        while (!ok_nesting) {
            // 1. Set candidateAncestor to the result of running nearest inclusive open popover given candidate.
            candidate_ancestor = candidate->nearest_inclusive_open_popover();

            // 2. If candidateAncestor is null or popoverPositions does not contain candidateAncestor, then return.
            if (!candidate_ancestor || !popover_positions.contains(*candidate_ancestor))
                return;

            // 3. Assert: candidateAncestor's popover attribute is not in the manual or none state.
            VERIFY(!candidate_ancestor->popover().has_value() || candidate_ancestor->popover().value() != "manual"sv);

            // AD-HOC: This also checks if isPopover is false.
            // Spec issue: https://github.com/whatwg/html/issues/11008.
            // 4. Set okNesting to true if newPopoverOrTopLayerElement's popover attribute is in the hint state or candidateAncestor's popover attribute is in the auto state.
            if (is_popover == IsPopover::No || new_popover->popover() == "hint"sv || candidate_ancestor->popover() == "auto"sv)
                ok_nesting = true;

            // 5. If okNesting is false, then set candidate to candidateAncestor's parent in the flat tree.
            if (!ok_nesting)
                candidate = candidate_ancestor->shadow_including_first_ancestor_of_type<HTMLElement>();
        }

        // 5. Let candidatePosition be popoverPositions[candidateAncestor].
        auto candidate_position = popover_positions.get(*candidate_ancestor).value();

        // 6. If topmostPopoverAncestor is null or popoverPositions[topmostPopoverAncestor] is less than candidatePosition, then set topmostPopoverAncestor to candidateAncestor.
        if (!topmost_popover_ancestor || popover_positions.get(*topmost_popover_ancestor).value() < candidate_position)
            topmost_popover_ancestor = candidate_ancestor;
    };

    // 10. Run checkAncestor given newPopoverOrTopLayerElement's parent node within the flat tree.
    check_ancestor(new_popover_or_top_layer_element->shadow_including_first_ancestor_of_type<HTMLElement>());

    // 11. Run checkAncestor given invoker.
    check_ancestor(invoker.ptr());

    // 12. Return topmostPopoverAncestor.
    return topmost_popover_ancestor;
}

// https://html.spec.whatwg.org/multipage/popover.html#nearest-inclusive-open-popover
GC::Ptr<HTMLElement> HTMLElement::nearest_inclusive_open_popover()
{
    // To find the nearest inclusive open popover given a Node node, perform the following steps. They return an HTML element or null.

    // 1. Let currentNode be node.
    auto* current_node = this;

    // 2. While currentNode is not null:
    while (current_node) {
        // AD-HOC: This also allows hint popovers.
        // Spec issue: https://github.com/whatwg/html/issues/11008.
        // 1. If currentNode's popover attribute is in the auto state and currentNode's popover visibility state is showing, then return currentNode.
        if (current_node->popover().has_value() && current_node->popover().value().is_one_of("auto", "hint") && current_node->popover_visibility_state() == PopoverVisibilityState::Showing)
            return current_node;

        // 2. Set currentNode to currentNode's parent in the flat tree.
        current_node = current_node->shadow_including_first_ancestor_of_type<HTMLElement>();
    }

    // 3. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/popover.html#queue-a-popover-toggle-event-task
void HTMLElement::queue_a_popover_toggle_event_task(String old_state, String new_state)
{
    // 1. If element's popover toggle task tracker is not null, then:
    if (m_popover_toggle_task_tracker.has_value()) {
        // 1. Set oldState to element's popover toggle task tracker's old state.
        old_state = move(m_popover_toggle_task_tracker->old_state);

        // 2. Remove element's popover toggle task tracker's task from its task queue.
        HTML::main_thread_event_loop().task_queue().remove_tasks_matching([&](auto const& task) {
            return task.id() == m_popover_toggle_task_tracker->task_id;
        });

        // 3. Set element's popover toggle task tracker to null.
        m_popover_toggle_task_tracker->task_id = {};
    }

    // 2. Queue an element task given the DOM manipulation task source and element to run the following steps:
    auto task_id = queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, old_state, new_state = move(new_state)]() mutable {
        // 1. Fire an event named toggle at element, using ToggleEvent, with the oldState attribute initialized to
        //    oldState and the newState attribute initialized to newState.
        ToggleEventInit event_init {};
        event_init.old_state = move(old_state);
        event_init.new_state = move(new_state);

        dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::toggle, move(event_init)));

        // 2. Set element's popover toggle task tracker to null.
        m_popover_toggle_task_tracker = {};
    });

    // 3. Set element's popover toggle task tracker to a struct with task set to the just-queued task and old state set to oldState.
    m_popover_toggle_task_tracker = ToggleTaskTracker {
        .task_id = task_id,
        .old_state = move(old_state),
    };
}

void HTMLElement::did_receive_focus()
{
    if (!first_is_one_of(m_content_editable_state, ContentEditableState::True, ContentEditableState::PlaintextOnly))
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
    if (!first_is_one_of(m_content_editable_state, ContentEditableState::True, ContentEditableState::PlaintextOnly))
        return;

    document().editing_host_manager()->set_active_contenteditable_element(nullptr);
}

void HTMLElement::removed_from(Node* old_parent, Node& old_root)
{
    Element::removed_from(old_parent, old_root);

    // https://whatpr.org/html/9457/infrastructure.html#dom-trees:concept-node-remove-ext
    // If removedNode's popover attribute is not in the no popover state, then run the hide popover algorithm given removedNode, false, false, false, and true.
    if (popover().has_value())
        MUST(hide_popover(FocusPreviousElement::No, FireEvents::No, ThrowExceptions::No, IgnoreDomState::Yes));

    if (old_parent) {
        auto* parent_html_element = as_if<HTMLElement>(old_parent);
        if (!parent_html_element)
            parent_html_element = old_parent->first_ancestor_of_type<HTMLElement>();
        if (parent_html_element && parent_html_element->is_inert() && !has_attribute(HTML::AttributeNames::inert))
            set_subtree_inertness(false);
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-accesskeylabel
String HTMLElement::access_key_label() const
{
    dbgln("FIXME: Implement HTMLElement::access_key_label()");
    return String {};
}

}
