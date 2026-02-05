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
#include <LibWeb/DOM/Range.h>
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
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/PopoverTargetAttributes.h>
#include <LibWeb/HTML/ToggleEvent.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Selection/Selection.h>
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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLElement);
    Base::initialize(realm);
}

void HTMLElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    HTMLOrSVGElement::visit_edges(visitor);
    visitor.visit(m_labels);
    visitor.visit(m_attached_internals);
    visitor.visit(m_popover_trigger);
    visitor.visit(m_popover_close_watcher);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-translate
bool HTMLElement::translate() const
{
    // The translate IDL attribute must, on getting, return true if the element's translation mode is
    // translate-enabled, and false otherwise
    return translation_mode() == TranslationMode::TranslateEnabled;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-translate
void HTMLElement::set_translate(bool new_value)
{
    // On setting, it must set the content attribute's value to "yes" if the new value is true, and set the content
    // attribute's value to "no" otherwise.
    if (new_value)
        set_attribute_value(HTML::AttributeNames::translate, "yes"_string);
    else
        set_attribute_value(HTML::AttributeNames::translate, "no"_string);
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
    set_attribute_value(HTML::AttributeNames::dir, dir);
}

bool HTMLElement::is_focusable() const
{
    return Base::is_focusable() || is_editing_host();
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
        set_attribute_value(HTML::AttributeNames::contenteditable, "true"_string);
        return {};
    }
    if (content_editable.equals_ignoring_ascii_case("plaintext-only"sv)) {
        set_attribute_value(HTML::AttributeNames::contenteditable, "plaintext-only"_string);
        return {};
    }
    if (content_editable.equals_ignoring_ascii_case("false"sv)) {
        set_attribute_value(HTML::AttributeNames::contenteditable, "false"_string);
        return {};
    }
    return WebIDL::SyntaxError::create(realm(), "Invalid contentEditable value, must be 'true', 'false', 'plaintext-only' or 'inherit'"_utf16);
}

// https://html.spec.whatwg.org/multipage/dom.html#set-the-inner-text-steps
void HTMLElement::set_inner_text(Utf16View const& text)
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
    auto* text = as_if<DOM::Text>(next);
    if (!text)
        return;

    // 3. Replace data with node, node's data's length, 0, and next's data.
    MUST(node.replace_data(node.length_in_utf16_code_units(), 0, text->data()));

    // 4. Remove next.
    next->remove();
}

// https://html.spec.whatwg.org/multipage/dom.html#the-innertext-idl-attribute:dom-outertext-2
WebIDL::ExceptionOr<void> HTMLElement::set_outer_text(Utf16View const& value)
{
    // 1. If this's parent is null, then throw a "NoModificationAllowedError" DOMException.
    if (!parent())
        return WebIDL::NoModificationAllowedError::create(realm(), "setOuterText: parent is null"_utf16);

    // 2. Let next be this's next sibling.
    auto* next = next_sibling();

    // 3. Let previous be this's previous sibling.
    auto* previous = previous_sibling();

    // 4. Let fragment be the rendered text fragment for the given value given this's node document.
    auto fragment = rendered_text_fragment(value);

    // 5. If fragment has no children, then append a new Text node whose data is the empty string and node document is this's node document to fragment.
    if (!fragment->has_children())
        MUST(fragment->append_child(document().create_text_node({})));

    // 6. Replace this with fragment within this's parent.
    MUST(parent()->replace_child(fragment, *this));

    // 7. If next is non-null and next's previous sibling is a Text node, then merge with the next text node given next's previous sibling.
    if (next && is<DOM::Text>(next->previous_sibling()))
        merge_with_the_next_text_node(static_cast<DOM::Text&>(*next->previous_sibling()));

    // 8. If previous is a Text node, then merge with the next text node given previous.
    if (auto* previous_text = as_if<DOM::Text>(previous))
        merge_with_the_next_text_node(*previous_text);

    set_needs_style_update(true);
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#rendered-text-fragment
GC::Ref<DOM::DocumentFragment> HTMLElement::rendered_text_fragment(Utf16View const& input)
{
    // 1. Let fragment be a new DocumentFragment whose node document is document.
    auto fragment = realm().create<DOM::DocumentFragment>(document());

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    size_t position = 0;

    // 3. Let text be the empty string.
    // 4. While position is not past the end of input:
    while (position < input.length_in_code_units()) {
        auto start = position;

        // 1. Collect a sequence of code points that are not U+000A LF or U+000D CR from input given position, and set
        //    text to the result.
        while (position < input.length_in_code_units() && !first_is_one_of(input.code_unit_at(position), u'\n', u'\r'))
            ++position;

        auto text = input.substring_view(start, position - start);

        // 2. If text is not the empty string, then append a new Text node whose data is text and node document is
        //    document to fragment.
        if (!text.is_empty()) {
            MUST(fragment->append_child(document().create_text_node(Utf16String::from_utf16(text))));
        }

        // 3. While position is not past the end of input, and the code point at position is either U+000A LF or U+000D CR:
        while (position < input.length_in_code_units() && first_is_one_of(input.code_unit_at(position), u'\n', u'\r')) {
            // 1. If the code point at position is U+000D CR and the next code point is U+000A LF, then advance position
            //    to the next code point in input.
            if (input.code_unit_at(position) == '\r') {
                if (position + 1 < input.length_in_code_units() && input.code_unit_at(position + 1) == '\n')
                    ++position;
            }

            // 2. Advance position to the next code point in input.
            ++position;

            // 3. Append the result of creating an element given document, "br", and the HTML namespace to fragment.
            auto br_element = MUST(DOM::create_element(document(), HTML::TagNames::br, Namespace::HTML));
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
static Vector<Variant<Utf16String, RequiredLineBreakCount>> rendered_text_collection_steps(DOM::Node const& node)
{
    // 1. Let items be the result of running the rendered text collection steps with each child node of node in tree
    //    order, and then concatenating the results to a single list.
    Vector<Variant<Utf16String, RequiredLineBreakCount>> items;
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
    //    FIXME: - option elements have an associated non-replaced block-level CSS box whose child boxes are as normal for non-replaced block-level CSS boxes.
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

    // 4. If node is a Text node, then for each CSS text box produced by node, in content order, compute the text of the
    //    box after application of the CSS 'white-space' processing rules and 'text-transform' rules, set items to the
    //    list of the resulting strings, and return items.

    //    FIXME: The CSS 'white-space' processing rules are slightly modified: collapsible spaces at the end of lines are
    //    always collapsed, but they are only removed if the line is the last line of the block, or it ends with a br
    //    element. Soft hyphens should be preserved. [CSSTEXT]

    if (auto const* layout_text_node = as_if<Layout::TextNode>(layout_node)) {
        Layout::TextNode::ChunkIterator iterator { *layout_text_node, false, false };
        while (true) {
            auto chunk = iterator.next();
            if (!chunk.has_value())
                break;
            items.append(Utf16String::from_utf16(chunk.release_value().view));
        }
        return items;
    }

    // 5. If node is a br element, then append a string containing a single U+000A LF code point to items.
    if (is<HTML::HTMLBRElement>(node)) {
        items.append("\n"_utf16);
        return items;
    }

    auto display = computed_values.display();

    // 6. If node's computed value of 'display' is 'table-cell', and node's CSS box is not the last 'table-cell' box of its enclosing 'table-row' box, then append a string containing a single U+0009 TAB code point to items.
    if (display.is_table_cell() && node.next_sibling())
        items.append("\t"_utf16);

    // 7. If node's computed value of 'display' is 'table-row', and node's CSS box is not the last 'table-row' box of the nearest ancestor 'table' box, then append a string containing a single U+000A LF code point to items.
    if (display.is_table_row() && node.next_sibling())
        items.append("\n"_utf16);

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
Utf16String HTMLElement::get_the_text_steps()
{
    // 1. If element is not being rendered or if the user agent is a non-CSS user agent, then return element's descendant text content.
    document().update_layout(DOM::UpdateLayoutReason::HTMLElementGetTheTextSteps);
    if (!layout_node())
        return descendant_text_content();

    // 2. Let results be a new empty list.
    Vector<Variant<Utf16String, RequiredLineBreakCount>> results;

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
            [](Utf16String const& string) { return string.is_empty(); },
            [](RequiredLineBreakCount const&) { return false; });
    });

    // 5. Remove any runs of consecutive required line break count items at the start or end of results.
    while (!results.is_empty() && results.first().has<RequiredLineBreakCount>())
        results.take_first();
    while (!results.is_empty() && results.last().has<RequiredLineBreakCount>())
        results.take_last();

    // 6. Replace each remaining run of consecutive required line break count items with a string consisting of as many
    //    U+000A LF code points as the maximum of the values in the required line break count items.
    StringBuilder builder(StringBuilder::Mode::UTF16);
    for (size_t i = 0; i < results.size(); ++i) {
        results[i].visit(
            [&](Utf16String const& string) {
                builder.append(string);
            },
            [&](RequiredLineBreakCount const& line_break_count) {
                int max_line_breaks = line_break_count.count;
                size_t j = i + 1;
                while (j < results.size() && results[j].has<RequiredLineBreakCount>()) {
                    max_line_breaks = max(max_line_breaks, results[j].get<RequiredLineBreakCount>().count);
                    ++j;
                }

                // Skip over the run of required line break counts.
                i = j - 1;

                builder.append_repeated('\n', max_line_breaks);
            });
    }

    // 7. Return the concatenation of the string items in results.
    return builder.to_utf16_string();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-innertext
Utf16String HTMLElement::inner_text()
{
    // The innerText and outerText getter steps are to return the result of running get the text steps with this.
    return get_the_text_steps();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-outertext
Utf16String HTMLElement::outer_text()
{
    // The innerText and outerText getter steps are to return the result of running get the text steps with this.
    return get_the_text_steps();
}

static bool any_ancestor_establishes_a_fixed_position_containing_block(Layout::NodeWithStyle const& node)
{
    // https://www.w3.org/TR/css-position-3/#fixed-positioning-containing-block
    // The containing block is established by the nearest ancestor box that establishes an fixed positioning containing
    // block, with the bounds of the containing block determined identically to the absolute positioning containing block.
    for (auto ancestor = node.containing_block(); ancestor; ancestor = ancestor->containing_block()) {
        if (ancestor->establishes_a_fixed_positioning_containing_block())
            return true;
    }
    return false;
}

// https://drafts.csswg.org/cssom-view/#dom-htmlelement-scrollparent
GC::Ptr<DOM::Element> HTMLElement::scroll_parent() const
{
    // NOTE: We have to ensure that the layout is up-to-date before querying the layout tree.
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementScrollParent);

    // 1. If any of the following holds true, return null and terminate this algorithm:
    //    - The element does not have an associated box.
    //    - The element is the root element.
    //    - The element is the body element.
    //    - The element’s computed value of the position property is fixed and no ancestor establishes a fixed position containing block.
    if (!layout_node())
        return nullptr;
    if (is_document_element())
        return nullptr;
    if (is_html_body_element())
        return nullptr;
    bool const no_ancestor_establishes_a_fixed_position_containing_block = !any_ancestor_establishes_a_fixed_position_containing_block(*layout_node());
    if (layout_node()->is_fixed_position() && no_ancestor_establishes_a_fixed_position_containing_block)
        return nullptr;

    // 2. Let ancestor be the containing block of the element in the flat tree and repeat these substeps:
    auto ancestor = layout_node()->containing_block();
    while (ancestor) {
        // 1. If ancestor is the initial containing block, return the scrollingElement for the element’s document if it
        //    is not closed-shadow-hidden from the element, otherwise return null.
        if (ancestor->is_viewport()) {
            auto const scrolling_element = document().scrolling_element();
            if (scrolling_element && !scrolling_element->is_closed_shadow_hidden_from(*this))
                return const_cast<Element*>(scrolling_element.ptr());
            return nullptr;
        }

        // 2. If ancestor is not closed-shadow-hidden from the element, and is a scroll container, terminate this
        //    algorithm and return ancestor.
        if ((ancestor->dom_node() && !ancestor->dom_node()->is_closed_shadow_hidden_from(*this))
            && ancestor->is_scroll_container()) {
            return const_cast<Element*>(static_cast<DOM::Element const*>(ancestor->dom_node()));
        }

        // 3. If the computed value of the position property of ancestor is fixed, and no ancestor establishes a fixed
        //    position containing block, terminate this algorithm and return null.
        if (ancestor->computed_values().position() == CSS::Positioning::Fixed && no_ancestor_establishes_a_fixed_position_containing_block)
            return nullptr;

        // 4. Let ancestor be the containing block of ancestor in the flat tree.
        ancestor = ancestor->containing_block();
    }

    return nullptr;
}

// https://drafts.csswg.org/cssom-view-1/#dom-htmlelement-offsetparent
GC::Ptr<DOM::Element> HTMLElement::offset_parent() const
{
    // NOTE: We have to ensure that the layout is up-to-date before querying the layout tree.
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLElementOffsetParent);

    // 1. If any of the following holds true return null and terminate this algorithm:
    //    - The element does not have an associated box.
    //    - The element is the root element.
    //    - The element is the HTML body element.
    //    - The element’s computed value of the position property is fixed and no ancestor establishes a fixed position containing block.
    if (!layout_node())
        return nullptr;
    if (is_document_element())
        return nullptr;
    if (is_html_body_element())
        return nullptr;
    bool const no_ancestor_establishes_a_fixed_position_containing_block = !any_ancestor_establishes_a_fixed_position_containing_block(*layout_node());
    if (layout_node()->is_fixed_position() && no_ancestor_establishes_a_fixed_position_containing_block)
        return nullptr;

    // 2. Let ancestor be the parent of the element in the flat tree and repeat these substeps:
    auto ancestor = shadow_including_first_ancestor_of_type<DOM::Element>();
    while (true) {
        // 1. If ancestor is closed-shadow-hidden from the element, its computed value of the position property is
        //    fixed, and no ancestor establishes a fixed position containing block, terminate this algorithm and return
        //    null.
        bool ancestor_is_closed_shadow_hidden = ancestor->is_closed_shadow_hidden_from(*this);
        if (ancestor_is_closed_shadow_hidden
            && ancestor->computed_properties()->position() == CSS::Positioning::Fixed
            && no_ancestor_establishes_a_fixed_position_containing_block)
            return nullptr;

        // 2. If ancestor is not closed-shadow-hidden from the element and satisfies at least one of the following,
        //    terminate this algorithm and return ancestor.
        if (!ancestor_is_closed_shadow_hidden) {
            // - The element is in a fixed position containing block, and ancestor is a containing block for
            //   fixed-positioned descendants.
            // FIXME: This is ambiguous but I believe it means any ancestor establishes a fixed position containing block.
            //        https://github.com/w3c/csswg-drafts/pull/12531/commits/48e905bb3859f80ce822299f7e6b76515d867fc3#r2623785087
            if (!no_ancestor_establishes_a_fixed_position_containing_block && ancestor->layout_node()->establishes_a_fixed_positioning_containing_block())
                return const_cast<Element*>(ancestor);
            // - The element is not in a fixed position containing block, and:
            if (no_ancestor_establishes_a_fixed_position_containing_block) {
                // - ancestor is a containing block of absolutely-positioned descendants (regardless of whether there
                //   are any absolutely-positioned descendants).
                if (ancestor->layout_node()->is_positioned())
                    return const_cast<Element*>(ancestor);
                // - It is the body element.
                if (ancestor->is_html_body_element())
                    return const_cast<Element*>(ancestor);
                // - The computed value of the position property of the element is static and the ancestor is one of
                //   the following HTML elements: td, th, or table.
                if (computed_properties()->position() == CSS::Positioning::Static && ancestor->local_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::table))
                    return const_cast<Element*>(ancestor);
            }
            // - FIXME: The element has a different effective zoom than ancestor.
        }

        // 3. If there is no more parent of ancestor in the flat tree, terminate this algorithm and return null.
        auto parent_of_ancestor = ancestor->shadow_including_first_ancestor_of_type<DOM::Element>();
        if (!parent_of_ancestor)
            return nullptr;

        // 4. Let ancestor be the parent of ancestor in the flat tree.
        ancestor = parent_of_ancestor;
    }
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
        //    then run the hide popover algorithm given element, true, true, false, true, and null.
        if (m_popover_visibility_state == PopoverVisibilityState::Showing
            && popover_value_to_state(old_value) != popover_value_to_state(value))
            MUST(hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::Yes, nullptr));
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

    if (auto paintable_box = this->paintable_box())
        paintable_box->set_needs_paint_only_properties_update(true);
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
            auto* label_element = as_if<HTMLLabelElement>(node);
            return label_element && label_element->control() == this;
        });
    }

    return m_labels;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-hidden
Variant<bool, double, String> HTMLElement::hidden() const
{
    // 1. If the hidden attribute is in the hidden until found state, then return "until-found".
    auto const& hidden = get_attribute(HTML::AttributeNames::hidden);
    if (hidden.has_value() && hidden->equals_ignoring_ascii_case("until-found"sv))
        return "until-found"_string;
    // 2. If the hidden attribute is set, then return true.
    if (hidden.has_value())
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
            set_attribute_value(HTML::AttributeNames::hidden, "until-found"_string);
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
    set_attribute_value(HTML::AttributeNames::hidden, ""_string);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-click
void HTMLElement::click()
{
    // 1. If this element is a form control that is disabled, then return.
    if (auto* form_control = as_if<FormAssociatedElement>(this)) {
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

// https://html.spec.whatwg.org/multipage/custom-elements.html#form-associated-custom-element
bool HTMLElement::is_form_associated_custom_element()
{
    // An autonomous custom element is called a form-associated custom element if the element is associated with a
    // custom element definition whose form-associated field is set to true.
    auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), is_value());
    return definition->form_associated();
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
        for (auto ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
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
        for (auto ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
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

WebIDL::ExceptionOr<GC::Ref<ElementInternals>> HTMLElement::attach_internals()
{
    // 1. If this's is value is not null, then throw a "NotSupportedError" DOMException.
    if (is_value().has_value())
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals cannot be attached to a customized built-in element"_utf16);

    // 2. Let definition be the result of looking up a custom element definition given this's node document, its namespace, its local name, and null as the is value.
    auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), is_value());

    // 3. If definition is null, then throw an "NotSupportedError" DOMException.
    if (!definition)
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals cannot be attached to an element that is not a custom element"_utf16);

    // 4. If definition's disable internals is true, then throw a "NotSupportedError" DOMException.
    if (definition->disable_internals())
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals are disabled for this custom element"_utf16);

    // 5. If this's attached internals is non-null, then throw an "NotSupportedError" DOMException.
    if (m_attached_internals)
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals already attached"_utf16);

    // 6. If this's custom element state is not "precustomized" or "custom", then throw a "NotSupportedError" DOMException.
    if (!first_is_one_of(custom_element_state(), DOM::CustomElementState::Precustomized, DOM::CustomElementState::Custom))
        return WebIDL::NotSupportedError::create(realm(), "Custom element is in an invalid state to attach ElementInternals"_utf16);

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
void HTMLElement::set_popover(Optional<String> value)
{
    // FIXME: This should probably be `Reflect` in the IDL.
    // The popover IDL attribute must reflect the popover attribute, limited to only known values.
    if (value.has_value())
        set_attribute_value(HTML::AttributeNames::popover, value.release_value());
    else
        remove_attribute(HTML::AttributeNames::popover);
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
    // 1. If ignoreDomState is false and element's popover attribute is in the No Popover state, then:
    if (ignore_dom_state == IgnoreDomState::No && !popover().has_value()) {
        // 1.1. If throwExceptions is true, then throw a "NotSupportedError" DOMException.
        if (throw_exceptions == ThrowExceptions::Yes)
            return WebIDL::NotSupportedError::create(realm(), "Element is not a popover"_utf16);
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
    // - element is a dialog element and its is modal flag is set to true; or
    // - FIXME: element's fullscreen flag is set,
    // then:
    // 3.1 If throwExceptions is true, then throw an "InvalidStateError" DOMException.
    // 3.2 Return false.

    if ((ignore_dom_state == IgnoreDomState::No && !is_connected())
        || !document().is_fully_active()
        || (ignore_dom_state == IgnoreDomState::No && expected_document && &document() != expected_document)
        || (is<HTMLDialogElement>(*this) && as<HTMLDialogElement>(*this).is_modal())) {
        if (throw_exceptions == ThrowExceptions::Yes)
            return WebIDL::InvalidStateError::create(realm(), "Element is not in a valid state to show a popover"_utf16);
        return false;
    }

    // 4. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-showpopover
WebIDL::ExceptionOr<void> HTMLElement::show_popover_for_bindings(ShowPopoverOptions const& options)
{
    // 1. Let source be options["source"] if it exists; otherwise, null.
    auto source = options.source;
    // 2. Run show popover given this, true, and source.
    return show_popover(ThrowExceptions::Yes, source);
}

// https://html.spec.whatwg.org/multipage/popover.html#show-popover
// https://whatpr.org/html/9457/popover.html#show-popover
WebIDL::ExceptionOr<void> HTMLElement::show_popover(ThrowExceptions throw_exceptions, GC::Ptr<HTMLElement> source)
{
    // 1. If the result of running check popover validity given element, false, throwExceptions, null and false is false, then return.
    if (!TRY(check_popover_validity(ExpectedToBeShowing::No, throw_exceptions, nullptr, IgnoreDomState::No)))
        return {};

    // 2. Let document be element's node document.
    auto& document = this->document();

    // 3. Assert: element's popover trigger is null.
    VERIFY(!m_popover_trigger);

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

    // 9. If the result of firing an event named beforetoggle, using ToggleEvent, with the cancelable attribute
    //    initialized to true, the oldState attribute initialized to "closed", the newState attribute initialized to
    //    "open" at element, and the source attribute initialized to source at element is false,
    //    then run cleanupShowingFlag and return.
    ToggleEventInit event_init {};
    event_init.old_state = "closed"_string;
    event_init.new_state = "open"_string;
    event_init.cancelable = true;
    event_init.source = source;
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

    // NB: Steps 14 and 15 are implemented inside step 17 instead, see note below.

    // 16. If originalType is the Auto state, then:
    if (original_type == "auto"sv) {
        // 1. Run close entire popover list given document's showing hint popover list, shouldRestoreFocus, and fireEvents.
        close_entire_popover_list(document.showing_hint_popover_list(), should_restore_focus, fire_events);

        // 2. Let ancestor be the result of running the topmost popover ancestor algorithm given element, document's showing auto popover list, source, and true.
        Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> ancestor = topmost_popover_ancestor(this, document.showing_auto_popover_list(), source, IsPopover::Yes);

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
        auto auto_ancestor = topmost_popover_ancestor(this, document.showing_auto_popover_list(), source, IsPopover::Yes);

        // 15. Let hintAncestor be the result of running the topmost popover ancestor algorithm given element, document's showing hint popover list, invoker, and true.
        auto hint_ancestor = topmost_popover_ancestor(this, document.showing_hint_popover_list(), source, IsPopover::Yes);

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
            // 1. If throwExceptions is true, then throw an "InvalidStateError" DOMException.
            if (throw_exceptions == ThrowExceptions::Yes)
                return WebIDL::InvalidStateError::create(realm(), "Element is not in a valid state to show a popover"_utf16);

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
        // - cancelAction being to return true.
        // NB: We simply don't add an event listener for the cancel action.
        // - closeAction being to hide a popover given element, true, true, false, and null.
        auto close_callback_function = JS::NativeFunction::create(
            realm(), [this](JS::VM&) {
                MUST(hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::No, nullptr));

                return JS::js_undefined();
            },
            0, Utf16FlyString {}, &realm());
        auto close_callback = realm().heap().allocate<WebIDL::CallbackType>(*close_callback_function, realm());
        // - getEnabledState being to return true.
        auto get_enabled_state = GC::create_function(heap(), [] { return true; });

        m_popover_close_watcher = CloseWatcher::establish(*document.window(), move(get_enabled_state));
        m_popover_close_watcher->add_event_listener_without_options(HTML::EventNames::close, DOM::IDLEventListener::create(realm(), close_callback));
    }
    // FIXME: 19. Set element's previously focused element to null.
    // FIXME: 20. Let originallyFocusedElement be document's focused area of the document's DOM anchor.
    // 21. Add an element to the top layer given element.
    document.add_an_element_to_the_top_layer(*this);
    // 22. Set element's popover visibility state to showing.
    m_popover_visibility_state = PopoverVisibilityState::Showing;
    // 23. Set element's popover trigger to source.
    m_popover_trigger = source;
    // FIXME: 24. Set element's implicit anchor element to source.
    // FIXME: 25. Run the popover focusing steps given element.
    // FIXME: 26. If shouldRestoreFocus is true and element's popover attribute is not in the No Popover state, then set element's previously focused element to originallyFocusedElement.
    // 27. Queue a popover toggle event task given element, "closed", "open", and source.
    queue_a_popover_toggle_event_task("closed"_string, "open"_string, source);
    // 28. Run cleanupShowingFlag.
    cleanup_showing_flag();

    return {};
}

// https://html.spec.whatwg.org/multipage/popover.html#dom-hidepopover
// https://whatpr.org/html/9457/popover.html#dom-hidepopover
WebIDL::ExceptionOr<void> HTMLElement::hide_popover_for_bindings()
{
    // The hidePopover() method steps are to run the hide popover algorithm given this, true, true, true, false, and null.
    return hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::Yes, IgnoreDomState::No, nullptr);
}

// https://html.spec.whatwg.org/multipage/popover.html#hide-popover-algorithm
// https://whatpr.org/html/9457/popover.html#hide-popover-algorithm
WebIDL::ExceptionOr<void> HTMLElement::hide_popover(FocusPreviousElement focus_previous_element, FireEvents fire_events, ThrowExceptions throw_exceptions, IgnoreDomState ignore_dom_state, GC::Ptr<HTMLElement> source)
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

    // 7. If element's opened in popover mode is "auto" or "hint", then:
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

    // 9. If fireEvents is true:
    if (fire_events == FireEvents::Yes) {
        // 1. Fire an event named beforetoggle, using ToggleEvent, with the oldState attribute initialized to "open",
        //    the newState attribute initialized to "closed", and the source attribute set to source at element.
        ToggleEventInit event_init {};
        event_init.old_state = "open"_string;
        event_init.new_state = "closed"_string;
        event_init.source = source;
        dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::beforetoggle, move(event_init)));

        // 2. If autoPopoverListContainsElement is true and document's showing auto popover list's last item is not
        //    element, then run hide all popovers until given element, focusPreviousElement, and false.
        if (auto_popover_list_contains_element && (showing_popovers.is_empty() || showing_popovers.last() != this))
            hide_all_popovers_until(GC::Ptr(this), focus_previous_element, FireEvents::No);

        // 3. If the result of running check popover validity given element, true, throwExceptions, null, and
        //    ignoreDomState is false, then run cleanupSteps and return.
        if (!TRY(check_popover_validity(ExpectedToBeShowing::Yes, throw_exceptions, nullptr, ignore_dom_state))) {
            cleanup_steps();
            return {};
        }
        // 9.4. Request an element to be removed from the top layer given element.
        document.request_an_element_to_be_remove_from_the_top_layer(*this);
    }
    // 10. Otherwise, remove an element from the top layer immediately given element.
    else {
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

    // 11. Set element's popover trigger to null.
    m_popover_trigger = nullptr;

    // 12. Set element's opened in popover mode to null.
    m_opened_in_popover_mode = {};

    // 13. Set element's popover visibility state to hidden.
    m_popover_visibility_state = PopoverVisibilityState::Hidden;

    // 14. If fireEvents is true, then queue a popover toggle event task given element, "open", "closed", and source.
    if (fire_events == FireEvents::Yes)
        queue_a_popover_toggle_event_task("open"_string, "closed"_string, source);

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
    GC::Ptr<HTMLElement> source;

    // 2. If options is a boolean, set force to options.
    options.visit(
        [&force](bool forceBool) {
            force = forceBool;
        },
        [&force, &source](TogglePopoverOptions options) {
            // 3. Otherwise, if options["force"] exists, set force to options["force"].
            force = options.force;
            // 4. Let source be options["source"] if it exists; otherwise, null.
            source = options.source;
        });

    // 5. If this's popover visibility state is showing, and force is null or false, then run the hide popover algorithm given this, true, true, true, false, and null.
    if (popover_visibility_state() == PopoverVisibilityState::Showing && (!force.has_value() || !force.value()))
        TRY(hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::Yes, IgnoreDomState::No, nullptr));
    // 6. Otherwise, if force is not present or true, then run show popover given this true, and source.
    else if (!force.has_value() || force.value())
        TRY(show_popover(ThrowExceptions::Yes, source));
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

            // 2. Run the hide popover algorithm given the last item in popoverList, focusPreviousElement, fireEvents, false, and null.
            MUST(popover_list.last()->hide_popover(focus_previous_element, fire_events, ThrowExceptions::No, IgnoreDomState::No, nullptr));
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
        // 1. Run the hide popover algorithm given popoverList's last item, focusPreviousElement, fireEvents, false, and null.
        MUST(popover_list.last()->hide_popover(focus_previous_element, fire_events, ThrowExceptions::No, IgnoreDomState::No, nullptr));
    }
}

// https://html.spec.whatwg.org/multipage/popover.html#topmost-popover-ancestor
GC::Ptr<HTMLElement> HTMLElement::topmost_popover_ancestor(GC::Ptr<DOM::Node> new_popover_or_top_layer_element, Vector<GC::Ref<HTMLElement>> const& popover_list, GC::Ptr<HTMLElement> source, IsPopover is_popover)
{
    // To find the topmost popover ancestor, given a Node newPopoverOrTopLayerElement, a list popoverList, an HTML
    // element or null source, and a boolean isPopover, perform the following steps. They return an HTML element or null.

    // 1. If isPopover is true:
    auto* new_popover = as_if<HTML::HTMLElement>(*new_popover_or_top_layer_element);
    if (is_popover == IsPopover::Yes) {
        // 1. Assert: newPopoverOrTopLayerElement is an HTML element.
        VERIFY(new_popover);

        // 2. Assert: newPopoverOrTopLayerElement's popover attribute is not in the No Popover state or the manual state.
        VERIFY(!new_popover->popover().has_value() || new_popover->popover().value() != "manual"sv);

        // 3. Assert: newPopoverOrTopLayerElement's popover visibility state is not in the popover showing state.
        VERIFY(new_popover->popover_visibility_state() != PopoverVisibilityState::Showing);
    }
    // 2. Otherwise:
    else {
        // 1. Assert: source is null.
        VERIFY(!source);
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

            // 4. Set okNesting to true if isPopover is false, newPopoverOrTopLayerElement's popover attribute is in the hint state, or candidateAncestor's popover attribute is in the auto state.
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

    // 11. Run checkAncestor given source.
    check_ancestor(source.ptr());

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
        // 1. If currentNode's popover attribute is in the Auto state or the Hint state, and currentNode's popover visibility state is showing, then return currentNode.
        if (current_node->popover().has_value() && current_node->popover().value().is_one_of("auto", "hint") && current_node->popover_visibility_state() == PopoverVisibilityState::Showing)
            return current_node;

        // 2. Set currentNode to currentNode's parent in the flat tree.
        current_node = current_node->shadow_including_first_ancestor_of_type<HTMLElement>();
    }

    // 3. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/popover.html#nearest-inclusive-target-popover
GC::Ptr<HTMLElement> HTMLElement::nearest_inclusive_target_popover()
{
    // To find the nearest inclusive target popover given a Node node:

    // 1. Let currentNode be node.
    auto* current_node = this;

    // 2. While currentNode is not null:
    while (current_node) {
        // 1. Let targetPopover be currentNode's popover target element.
        auto target_popover = PopoverTargetAttributes::get_the_popover_target_element(*current_node);

        // 2. If targetPopover is not null and targetPopover's popover attribute is in the Auto state or the Hint state, and targetPopover's popover visibility state is showing, then return targetPopover.
        if (target_popover) {
            if (target_popover->popover().has_value() && target_popover->popover().value().is_one_of("auto", "hint") && target_popover->popover_visibility_state() == PopoverVisibilityState::Showing)
                return target_popover;
        }

        // 3. Set currentNode to currentNode's ancestor in the flat tree.
        current_node = current_node->shadow_including_first_ancestor_of_type<HTMLElement>();
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/popover.html#queue-a-popover-toggle-event-task
void HTMLElement::queue_a_popover_toggle_event_task(String old_state, String new_state, GC::Ptr<HTMLElement> source)
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
    auto task_id = queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, old_state, new_state = move(new_state), source]() mutable {
        // 1. Fire an event named toggle at element, using ToggleEvent, with the oldState attribute initialized to
        //    oldState, the newState attribute initialized to newState, and the source attribute initialized to source.
        ToggleEventInit event_init {};
        event_init.old_state = move(old_state);
        event_init.new_state = move(new_state);
        event_init.source = source;

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

// https://html.spec.whatwg.org/multipage/popover.html#light-dismiss-open-popovers
void HTMLElement::light_dismiss_open_popovers(UIEvents::PointerEvent const& event, GC::Ptr<DOM::Node> const target)
{
    // To light dismiss open popovers, given a PointerEvent event:

    // 1. Assert: event's isTrusted attribute is true.
    VERIFY(event.is_trusted());

    // 2. Let target be event's target.
    // FIXME: The event's target hasn't been initialized yet, so it's passed as an argument

    // 3. Let document be target's node document.
    auto& document = target->document();

    // 4. Let topmostPopover be the result of running topmost auto popover given document.
    auto topmost_popover = document.topmost_auto_or_hint_popover();

    // 5. If topmostPopover is null, then return.
    if (!topmost_popover)
        return;

    // 6. If event's type is "pointerdown", then: set document's popover pointerdown target to the result of running topmost clicked popover given target.
    if (event.type() == UIEvents::EventNames::pointerdown)
        document.set_popover_pointerdown_target(topmost_clicked_popover(target));

    // 7. If event's type is "pointerup", then:
    if (event.type() == UIEvents::EventNames::pointerup) {
        // 1. Let ancestor be the result of running topmost clicked popover given target.
        auto const ancestor = topmost_clicked_popover(target);

        // 2. Let sameTarget be true if ancestor is document's popover pointerdown target.
        bool const same_target = ancestor == document.popover_pointerdown_target();

        // 3. Set document's popover pointerdown target to null.
        document.set_popover_pointerdown_target({});

        // 4. If ancestor is null, then set ancestor to document.
        Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> ancestor_or_document = ancestor;
        if (!ancestor)
            ancestor_or_document = GC::Ptr(document);

        // 5. If sameTarget is true, then run hide all popovers until given ancestor, false, and true.
        if (same_target)
            hide_all_popovers_until(ancestor_or_document, FocusPreviousElement::No, FireEvents::Yes);
    }
}

// https://html.spec.whatwg.org/multipage/popover.html#get-the-popover-stack-position
size_t HTMLElement::popover_stack_position()
{
    // To get the popover stack position, given an HTML element popover:

    // 1. Let hintList be popover's node document's showing hint popover list.
    auto const& hint_list = document().showing_hint_popover_list();

    // 2. Let autoList be popover's node document's showing auto popover list.
    auto const& auto_list = document().showing_auto_popover_list();

    // 3. If popover is in hintList, then return the index of popover in hintList + the size of autoList + 1.
    if (hint_list.contains_slow(GC::Ref(*this)))
        return hint_list.find_first_index(GC::Ref(*this)).value() + auto_list.size() + 1;

    // 4. If popover is in autoList, then return the index of popover in autoList + 1.
    if (auto_list.contains_slow(GC::Ref(*this)))
        return auto_list.find_first_index(GC::Ref(*this)).value() + 1;

    // 5. Return 0.
    return 0;
}

// https://html.spec.whatwg.org/multipage/popover.html#topmost-clicked-popover
GC::Ptr<HTMLElement> HTMLElement::topmost_clicked_popover(GC::Ptr<DOM::Node> node)
{
    // To find the topmost clicked popover, given a Node node:

    GC::Ptr<HTMLElement> nearest_element = as_if<HTMLElement>(*node);
    if (!nearest_element)
        nearest_element = node->shadow_including_first_ancestor_of_type<HTMLElement>();

    if (!nearest_element)
        return {};

    // 1. Let clickedPopover be the result of running nearest inclusive open popover given node.
    auto clicked_popover = nearest_element->nearest_inclusive_open_popover();

    // 2. Let targetPopover be the result of running nearest inclusive target popover given node.
    auto target_popover = nearest_element->nearest_inclusive_target_popover();

    if (!clicked_popover)
        return target_popover;

    if (!target_popover)
        return clicked_popover;

    // 3. If the result of getting the popover stack position given clickedPopover is greater than the result of
    //    getting the popover stack position given targetPopover, then return clickedPopover.
    if (clicked_popover->popover_stack_position() > target_popover->popover_stack_position())
        return clicked_popover;

    // 4. Return targetPopover.
    return target_popover;
}

void HTMLElement::did_receive_focus()
{
    if (!first_is_one_of(m_content_editable_state, ContentEditableState::True, ContentEditableState::PlaintextOnly))
        return;

    auto editing_host = document().editing_host_manager();
    editing_host->set_active_contenteditable_element(this);

    // Don't update the selection if we're already part of the active range.
    if (auto range = document().get_selection()->range()) {
        if (is_inclusive_ancestor_of(range->start_container()) || is_inclusive_ancestor_of(range->end_container()))
            return;
    }

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

    // https://html.spec.whatwg.org/multipage/infrastructure.html#dom-trees:concept-node-remove-ext
    // If removedNode's popover attribute is not in the No Popover state, then run the hide popover algorithm given removedNode, false, false, false, true, and null.
    if (popover().has_value())
        MUST(hide_popover(FocusPreviousElement::No, FireEvents::No, ThrowExceptions::No, IgnoreDomState::Yes, nullptr));

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

// https://html.spec.whatwg.org/multipage/dnd.html#dom-draggable
bool HTMLElement::draggable() const
{
    auto attribute = get_attribute(HTML::AttributeNames::draggable);

    // If an element's draggable content attribute has the state True, the draggable IDL attribute must return true.
    if (attribute.has_value() && attribute->equals_ignoring_ascii_case("true"sv)) {
        return true;
    }

    // If an element's draggable content attribute has the state False, the draggable IDL attribute must return false.
    if (attribute.has_value() && attribute->equals_ignoring_ascii_case("false"sv)) {
        return false;
    }

    // Otherwise, the element's draggable content attribute has the state Auto.

    // If the element is an img element, the draggable IDL attribute must return true.
    if (is<HTML::HTMLImageElement>(*this)) {
        return true;
    }

    // If the element is an object element that represents an image, the draggable IDL attribute must return true.
    if (is<HTML::HTMLObjectElement>(*this)) {
        if (auto type_attribute = get_attribute(HTML::AttributeNames::type); type_attribute.has_value() && type_attribute->equals_ignoring_ascii_case("image"sv))
            return true;
    }

    // If the element is an a element with an href content attribute, the draggable IDL attribute must return true.
    if (is<HTML::HTMLAnchorElement>(*this) && has_attribute(HTML::AttributeNames::href)) {
        return true;
    }

    // Otherwise, the draggable IDL attribute must return false.
    return false;
}

void HTMLElement::set_draggable(bool draggable)
{
    set_attribute_value(HTML::AttributeNames::draggable, draggable ? "true"_string : "false"_string);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-spellcheck
bool HTMLElement::spellcheck() const
{
    // The spellcheck attribute is an enumerated attribute with the following keywords and states:
    // Keyword            | State | Brief description
    // true               | True  | Spelling and grammar will be checked.
    // (the empty string) |       |
    // false              | False | and grammar will not be checked.

    // The attribute's missing value default and invalid value default are both the Default state. The default state
    // indicates that the element is to act according to a default behavior, possibly based on the parent element's
    // own spellcheck state, as defined below.

    // For each element, user agents must establish a default behavior, either through defaults or through preferences
    // expressed by the user. There are three possible default behaviors for each element:

    // true-by-default
    //     The element will be checked for spelling and grammar if its contents are editable and spellchecking is not
    //     explicitly disabled through the spellcheck attribute.
    // false-by-default
    //     The element will never be checked for spelling and grammar unless spellchecking is explicitly enabled
    //     through the spellcheck attribute.
    // inherit-by-default
    //     The element's default behavior is the same as its parent element's. Elements that have no parent element
    //     cannot have this as their default behavior.

    // NOTE: We use "true-by-default" for elements which are editable, editing hosts, or form associated text control
    //       elements "false-by-default" for root elements, and "inherit-by-default" for other elements.

    auto maybe_spellcheck_attribute = attribute(HTML::AttributeNames::spellcheck);

    // The spellcheck IDL attribute, on getting, must return true if the element's spellcheck content attribute is in the True state,
    if (maybe_spellcheck_attribute.has_value() && (maybe_spellcheck_attribute.value().equals_ignoring_ascii_case("true"sv) || maybe_spellcheck_attribute.value().is_empty()))
        return true;

    if (!maybe_spellcheck_attribute.has_value() || !maybe_spellcheck_attribute.value().equals_ignoring_ascii_case("false"sv)) {
        // or if the element's spellcheck content attribute is in the Default state and the element's default behavior is true-by-default,
        if (is_editable_or_editing_host() || is<FormAssociatedTextControlElement>(this))
            return true;

        // or if the element's spellcheck content attribute is in the Default state and the element's default behavior is inherit-by-default
        if (auto* parent_html_element = first_ancestor_of_type<HTMLElement>()) {
            // and the element's parent element's spellcheck IDL attribute would return true;
            if (parent_html_element->spellcheck())
                return true;
        }
    }

    // if none of those conditions applies, then the attribute must instead return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-spellcheck
void HTMLElement::set_spellcheck(bool spellcheck)
{
    // On setting, if the new value is true, then the element's spellcheck content attribute must be set to "true", otherwise it must be set to "false".
    if (spellcheck)
        set_attribute_value(HTML::AttributeNames::spellcheck, "true"_string);
    else
        set_attribute_value(HTML::AttributeNames::spellcheck, "false"_string);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-writingsuggestions
String HTMLElement::writing_suggestions() const
{
    // The writingsuggestions content attribute is an enumerated attribute with the following keywords and states:
    // Keyword            | State | Brief description
    // true               | True  | Writing suggestions should be offered on this element.
    // (the empty string) |       |
    // false              | False | Writing suggestions should not be offered on this element.

    // The attribute's missing value default is the Default state. The default state indicates that the element is to
    // act according to a default behavior, possibly based on the parent element's own writingsuggestions state, as
    // defined below.

    // The attribute's invalid value default is the True state.

    // 1. If element's writingsuggestions content attribute is in the False state, return "false".
    auto maybe_writing_suggestions_attribute = attribute(HTML::AttributeNames::writingsuggestions);

    if (maybe_writing_suggestions_attribute.has_value() && maybe_writing_suggestions_attribute.value().equals_ignoring_ascii_case("false"sv))
        return "false"_string;

    // 2. If element's writingsuggestions content attribute is in the Default state, element has a parent element, and the computed writing suggestions value of element's parent element is "false", then return "false".
    if (!maybe_writing_suggestions_attribute.has_value() && first_ancestor_of_type<HTMLElement>() && first_ancestor_of_type<HTMLElement>()->writing_suggestions() == "false"sv) {
        return "false"_string;
    }

    // 3. Return "true".
    return "true"_string;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-writingsuggestions
void HTMLElement::set_writing_suggestions(String const& given_value)
{
    // 1. Set this's writingsuggestions content attribute to the given value.
    set_attribute_value(HTML::AttributeNames::writingsuggestions, given_value);
}

// https://html.spec.whatwg.org/multipage/interaction.html#own-autocapitalization-hint
HTMLElement::AutocapitalizationHint HTMLElement::own_autocapitalization_hint() const
{
    // The autocapitalization processing model is based on selecting among five autocapitalization hints, defined as follows:
    //
    // Default
    //     The user agent and input method should make their own determination of whether or not to enable autocapitalization.
    // none
    //     No autocapitalization should be applied (all letters should default to lowercase).
    // Sentences
    //     The first letter of each sentence should default to a capital letter; all other letters should default to lowercase.
    // Words
    //     The first letter of each word should default to a capital letter; all other letters should default to lowercase.
    // Characters
    //     All letters should default to uppercase.

    // The autocapitalize attribute is an enumerated attribute whose states are the possible autocapitalization hints.
    // The autocapitalization hint specified by the attribute's state combines with other considerations to form the
    // used autocapitalization hint, which informs the behavior of the user agent. The keywords for this attribute and
    // their state mappings are as follows:

    // Keyword    | State
    // off        | None
    // none       |
    // on         | Sentences
    // sentences  |
    // words      | Words
    // characters | Characters

    // The attribute's missing value default is the Default state, and its invalid value default is the Sentences state.

    // To compute the own autocapitalization hint of an element element, run the following steps:
    // 1. If the autocapitalize content attribute is present on element, and its value is not the empty string, return the
    //    state of the attribute.
    auto maybe_autocapitalize_attribute = attribute(HTML::AttributeNames::autocapitalize);

    if (maybe_autocapitalize_attribute.has_value() && !maybe_autocapitalize_attribute.value().is_empty()) {
        auto autocapitalize_attribute_string_view = maybe_autocapitalize_attribute.value().bytes_as_string_view();

        if (autocapitalize_attribute_string_view.is_one_of_ignoring_ascii_case("off"sv, "none"sv))
            return AutocapitalizationHint::None;

        if (autocapitalize_attribute_string_view.equals_ignoring_ascii_case("words"sv))
            return AutocapitalizationHint::Words;

        if (autocapitalize_attribute_string_view.equals_ignoring_ascii_case("characters"sv))
            return AutocapitalizationHint::Characters;

        return AutocapitalizationHint::Sentences;
    }

    // 2. If element is an autocapitalize-and-autocorrect inheriting element and has a non-null form owner, return the
    //    own autocapitalization hint of element's form owner.
    auto const* form_associated_element = as_if<FormAssociatedElement>(this);
    if (form_associated_element && form_associated_element->is_autocapitalize_and_autocorrect_inheriting() && form_associated_element->form())
        return form_associated_element->form()->own_autocapitalization_hint();

    // 3. Return Default.
    return AutocapitalizationHint::Default;
}

// https://html.spec.whatwg.org/multipage/interaction.html#attr-autocapitalize
String HTMLElement::autocapitalize() const
{
    // The autocapitalize getter steps are to:
    // 1. Let state be the own autocapitalization hint of this.
    auto state = own_autocapitalization_hint();

    // 2. If state is Default, then return the empty string.
    // 3. If state is None, then return "none".
    // 4. If state is Sentences, then return "sentences".
    // 5. Return the keyword value corresponding to state.
    switch (state) {
    case AutocapitalizationHint::Default:
        return String {};
    case AutocapitalizationHint::None:
        return "none"_string;
    case AutocapitalizationHint::Sentences:
        return "sentences"_string;
    case AutocapitalizationHint::Words:
        return "words"_string;
    case AutocapitalizationHint::Characters:
        return "characters"_string;
    }

    VERIFY_NOT_REACHED();
}

void HTMLElement::set_autocapitalize(String const& given_value)
{
    // The autocapitalize setter steps are to set the autocapitalize content attribute to the given value.
    set_attribute_value(HTML::AttributeNames::autocapitalize, given_value);
}

// https://html.spec.whatwg.org/multipage/interaction.html#used-autocorrection-state
HTMLElement::AutocorrectionState HTMLElement::used_autocorrection_state() const
{
    // The autocorrect attribute is an enumerated attribute with the following keywords and states:
    // Keyword            | State | Brief description
    // on                 | On    | The user agent is permitted to automatically correct spelling errors while the user
    // (the empty string) |       | types. Whether spelling is automatically corrected while typing left is for the user
    //                    |       | agent to decide, and may depend on the element as well as the user's preferences.
    // off                | Off   | The user agent is not allowed to automatically correct spelling while the user types.

    // The attribute's invalid value default and missing value default are both the On state.

    auto autocorrect_attribute_state = [](Optional<String> attribute) {
        if (attribute.has_value() && attribute.value().equals_ignoring_ascii_case("off"sv))
            return AutocorrectionState::Off;

        return AutocorrectionState::On;
    };

    // To compute the used autocorrection state of an element element, run these steps:
    // 1. If element is an input element whose type attribute is in one of the URL, E-mail, or Password states, then
    //    return Off.
    if (auto const* input_element = as_if<HTMLInputElement>(this)) {
        if (first_is_one_of(input_element->type_state(), HTMLInputElement::TypeAttributeState::URL, HTMLInputElement::TypeAttributeState::Email, HTMLInputElement::TypeAttributeState::Password))
            return AutocorrectionState::Off;
    }

    // 2. If the autocorrect content attribute is present on element, then return the state of the attribute.
    auto maybe_autocorrect_attribute = attribute(HTML::AttributeNames::autocorrect);

    if (maybe_autocorrect_attribute.has_value())
        return autocorrect_attribute_state(maybe_autocorrect_attribute);

    // 3. If element is an autocapitalize-and-autocorrect inheriting element and has a non-null form owner, then return
    //    the state of element's form owner's autocorrect attribute.
    if (auto const* form_associated_element = as_if<FormAssociatedElement>(this)) {
        if (form_associated_element->is_autocapitalize_and_autocorrect_inheriting() && form_associated_element->form())
            return autocorrect_attribute_state(form_associated_element->form()->attribute(HTML::AttributeNames::autocorrect));
    }

    // 4. Return On.
    return AutocorrectionState::On;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-autocorrect
bool HTMLElement::autocorrect() const
{
    // The autocorrect getter steps are: return true if the element's used autocorrection state is On and false if the
    // element's used autocorrection state is Off.
    return used_autocorrection_state() == AutocorrectionState::On;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-autocorrect
void HTMLElement::set_autocorrect(bool given_value)
{
    // The setter steps are: if the given value is true, then the element's autocorrect attribute must be set to "on";
    // otherwise it must be set to "off".
    if (given_value)
        set_attribute_value(HTML::AttributeNames::autocorrect, "on"_string);
    else
        set_attribute_value(HTML::AttributeNames::autocorrect, "off"_string);
}

// https://html.spec.whatwg.org/multipage/sections.html#get-an-element's-computed-heading-level
WebIDL::UnsignedLong HTMLElement::computed_heading_level() const
{
    // 1. Let level be 0.
    auto level = 0u;

    // 2. If element's local name is h1, then set level to 1.
    if (local_name() == TagNames::h1)
        level = 1;
    // 3. If element's local name is h2, then set level to 2.
    else if (local_name() == TagNames::h2)
        level = 2;
    // 4. If element's local name is h3, then set level to 3.
    else if (local_name() == TagNames::h3)
        level = 3;
    // 5. If element's local name is h4, then set level to 4.
    else if (local_name() == TagNames::h4)
        level = 4;
    // 6. If element's local name is h5, then set level to 5.
    else if (local_name() == TagNames::h5)
        level = 5;
    // 7. If element's local name is h6, then set level to 6.
    else if (local_name() == TagNames::h6)
        level = 6;

    // 8. Assert: level is not 0.
    VERIFY(level != 0);

    // 9. Increment level by the result of getting an element's computed heading offset given element.
    level += computed_heading_offset();

    // 10. If level is greater than 9, then return 9.
    if (level > 9)
        return 9;

    // 11. Return level.
    return level;
}

WebIDL::UnsignedLong HTMLElement::computed_heading_offset() const
{
    // 1. Let offset be 0.
    auto offset = 0u;

    // 2. Let inclusiveAncestor be element.
    DOM::Node const* inclusive_ancestor = this;

    // 3. While inclusiveAncestor is not null:
    while (inclusive_ancestor) {
        // 1. Let nextOffset be 0.
        auto next_offset = 0u;

        // 2. If inclusiveAncestor is an HTML element and has a headingoffset attribute, then parse its value using the
        //    rules for parsing non-negative integers.
        //    If the result of parsing the value is not an error, then set nextOffset to that value.
        auto const* inclusive_ancestor_html_element = as_if<HTMLElement>(*inclusive_ancestor);
        if (inclusive_ancestor_html_element) {
            if (auto attribute = inclusive_ancestor_html_element->get_attribute(AttributeNames::headingoffset); attribute.has_value()) {
                if (auto heading_offset = parse_non_negative_integer(attribute.value()); heading_offset.has_value())
                    next_offset = *heading_offset;
            }
        }

        // 3. Increment offset by nextOffset.
        offset += next_offset;

        // 4. If inclusiveAncestor is an HTML element and has a headingreset attribute, then return offset.
        if (inclusive_ancestor_html_element && inclusive_ancestor_html_element->has_attribute(AttributeNames::headingreset))
            return offset;

        // 5. Set inclusiveAncestor to the parent node of inclusiveAncestor within the flat tree.
        // FIXME: Flat tree parent means following slots.
        inclusive_ancestor = inclusive_ancestor->parent();
    }

    // 4. Return offset.
    return offset;
}

}
