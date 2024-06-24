/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/LiveNodeList.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/DOMStringMap.h>
#include <LibWeb/HTML/ElementInternals.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/FocusEvent.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLElement);

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
    visitor.visit(m_dataset);
    visitor.visit(m_labels);
    visitor.visit(m_attached_internals);
}

JS::NonnullGCPtr<DOMStringMap> HTMLElement::dataset()
{
    if (!m_dataset)
        m_dataset = DOMStringMap::create(*this);
    return *m_dataset;
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
    return WebIDL::SyntaxError::create(realm(), "Invalid contentEditable value, must be 'true', 'false', or 'inherit'"_fly_string);
}

void HTMLElement::set_inner_text(StringView text)
{
    remove_all_children();
    MUST(append_child(document().create_text_node(MUST(String::from_utf8(text)))));

    set_needs_style_update(true);
}

// https://html.spec.whatwg.org/multipage/dom.html#the-innertext-idl-attribute:dom-outertext-2
WebIDL::ExceptionOr<void> HTMLElement::set_outer_text(String)
{
    dbgln("FIXME: Implement HTMLElement::set_outer_text()");
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#get-the-text-steps
String HTMLElement::get_the_text_steps()
{
    // FIXME: Implement this according to spec.

    StringBuilder builder;

    // innerText for element being rendered takes visibility into account, so force a layout and then walk the layout tree.
    document().update_layout();
    if (!layout_node())
        return text_content().value_or(String {});

    Function<void(Layout::Node const&)> recurse = [&](auto& node) {
        for (auto* child = node.first_child(); child; child = child->next_sibling()) {
            if (is<Layout::TextNode>(child))
                builder.append(verify_cast<Layout::TextNode>(*child).text_for_rendering());
            if (is<Layout::BreakNode>(child))
                builder.append('\n');
            recurse(*child);
        }
    };
    recurse(*layout_node());

    return MUST(builder.to_string());
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
JS::GCPtr<DOM::Element> HTMLElement::offset_parent() const
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

    // 2. If the offsetParent of the element is null
    //    return the y-coordinate of the top border edge of the first CSS layout box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors, and terminate this algorithm.
    auto offset_parent = this->offset_parent();
    if (!offset_parent || !offset_parent->layout_node()) {
        auto position = paintable()->box_type_agnostic_position();
        return position.y().to_int();
    }

    // 3. Return the result of subtracting the y-coordinate of the top padding edge
    //    of the first box associated with the offsetParent of the element
    //    from the y-coordinate of the top border edge of the first box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors.
    auto offset_parent_position = offset_parent->paintable()->box_type_agnostic_position();
    auto position = paintable()->box_type_agnostic_position();
    return position.y().to_int() - offset_parent_position.y().to_int();
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

    // 2. If the offsetParent of the element is null
    //    return the x-coordinate of the left border edge of the first CSS layout box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors, and terminate this algorithm.
    auto offset_parent = this->offset_parent();
    if (!offset_parent || !offset_parent->layout_node()) {
        auto position = paintable()->box_type_agnostic_position();
        return position.x().to_int();
    }

    // 3. Return the result of subtracting the x-coordinate of the left padding edge
    //    of the first CSS layout box associated with the offsetParent of the element
    //    from the x-coordinate of the left border edge of the first CSS layout box associated with the element,
    //    relative to the initial containing block origin,
    //    ignoring any transforms that apply to the element and its ancestors.
    auto offset_parent_position = offset_parent->paintable()->box_type_agnostic_position();
    auto position = paintable()->box_type_agnostic_position();
    return position.x().to_int() - offset_parent_position.x().to_int();
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

void HTMLElement::attribute_changed(FlyString const& name, Optional<String> const& value)
{
    Element::attribute_changed(name, value);

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

// https://html.spec.whatwg.org/multipage/interaction.html#dom-focus
void HTMLElement::focus()
{
    // 1. If the element is marked as locked for focus, then return.
    if (m_locked_for_focus)
        return;

    // 2. Mark the element as locked for focus.
    m_locked_for_focus = true;

    // 3. Run the focusing steps for the element.
    run_focusing_steps(this);

    // FIXME: 4. If the value of the preventScroll dictionary member of options is false,
    //           then scroll the element into view with scroll behavior "auto",
    //           block flow direction position set to an implementation-defined value,
    //           and inline base direction position set to an implementation-defined value.

    // 5. Unmark the element as locked for focus.
    m_locked_for_focus = false;
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
JS::GCPtr<DOM::NodeList> HTMLElement::labels()
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
    // FIXME: 1. If this element is a form control that is disabled, then return.

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

// https://html.spec.whatwg.org/multipage/interaction.html#dom-blur
void HTMLElement::blur()
{
    // The blur() method, when invoked, should run the unfocusing steps for the element on which the method was called.
    run_unfocusing_steps(this);

    // User agents may selectively or uniformly ignore calls to this method for usability reasons.
}

Optional<ARIA::Role> HTMLElement::default_role() const
{
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
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-i
    if (local_name() == TagNames::i)
        return ARIA::Role::generic;
    // https://www.w3.org/TR/html-aria/#el-main
    if (local_name() == TagNames::main)
        return ARIA::Role::main;
    // https://www.w3.org/TR/html-aria/#el-nav
    if (local_name() == TagNames::nav)
        return ARIA::Role::navigation;
    // https://www.w3.org/TR/html-aria/#el-samp
    if (local_name() == TagNames::samp)
        return ARIA::Role::generic;
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

WebIDL::ExceptionOr<JS::NonnullGCPtr<ElementInternals>> HTMLElement::attach_internals()
{
    // 1. If this's is value is not null, then throw a "NotSupportedError" DOMException.
    if (is_value().has_value())
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals cannot be attached to a customized build-in element"_fly_string);

    // 2. Let definition be the result of looking up a custom element definition given this's node document, its namespace, its local name, and null as the is value.
    auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), is_value());

    // 3. If definition is null, then throw an "NotSupportedError" DOMException.
    if (!definition)
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals cannot be attached to an element that is not a custom element"_fly_string);

    // 4. If definition's disable internals is true, then throw a "NotSupportedError" DOMException.
    if (definition->disable_internals())
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals are disabled for this custom element"_fly_string);

    // 5. If this's attached internals is non-null, then throw an "NotSupportedError" DOMException.
    if (m_attached_internals)
        return WebIDL::NotSupportedError::create(realm(), "ElementInternals already attached"_fly_string);

    // 6. If this's custom element state is not "precustomized" or "custom", then throw a "NotSupportedError" DOMException.
    if (!first_is_one_of(custom_element_state(), DOM::CustomElementState::Precustomized, DOM::CustomElementState::Custom))
        return WebIDL::NotSupportedError::create(realm(), "Custom element is in an invalid state to attach ElementInternals"_fly_string);

    // 7. Set this's attached internals to a new ElementInternals instance whose target element is this.
    auto internals = ElementInternals::create(realm(), *this);

    m_attached_internals = internals;

    // 8. Return this's attached internals.
    return { internals };
}

void HTMLElement::did_receive_focus()
{
    if (m_content_editable_state != ContentEditableState::True)
        return;
    auto navigable = document().navigable();
    if (!navigable)
        return;
    navigable->set_cursor_position(DOM::Position::create(realm(), *this, 0));
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-accesskeylabel
String HTMLElement::access_key_label() const
{
    dbgln("FIXME: Implement HTMLElement::access_key_label()");
    return String {};
}

}
