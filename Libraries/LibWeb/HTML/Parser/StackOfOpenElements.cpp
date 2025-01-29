/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/StackOfOpenElements.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

static Vector<FlyString> s_base_list { "applet"_fly_string, "caption"_fly_string, "html"_fly_string, "table"_fly_string, "td"_fly_string, "th"_fly_string, "marquee"_fly_string, "object"_fly_string, "template"_fly_string };

StackOfOpenElements::~StackOfOpenElements() = default;

void StackOfOpenElements::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_elements);
}

bool StackOfOpenElements::has_in_scope_impl(FlyString const& tag_name, Vector<FlyString> const& list) const
{
    for (auto const& element : m_elements.in_reverse()) {
        if (element->local_name() == tag_name)
            return true;
        if (list.contains_slow(element->local_name()))
            return false;
    }
    VERIFY_NOT_REACHED();
}

bool StackOfOpenElements::has_in_scope(FlyString const& tag_name) const
{
    return has_in_scope_impl(tag_name, s_base_list);
}

bool StackOfOpenElements::has_in_scope_impl(const DOM::Element& target_node, Vector<FlyString> const& list) const
{
    for (auto& element : m_elements.in_reverse()) {
        if (element.ptr() == &target_node)
            return true;
        if (list.contains_slow(element->local_name()))
            return false;
    }
    VERIFY_NOT_REACHED();
}

bool StackOfOpenElements::has_in_scope(const DOM::Element& target_node) const
{
    return has_in_scope_impl(target_node, s_base_list);
}

bool StackOfOpenElements::has_in_button_scope(FlyString const& tag_name) const
{
    auto list = s_base_list;
    list.append("button"_fly_string);
    return has_in_scope_impl(tag_name, list);
}

bool StackOfOpenElements::has_in_table_scope(FlyString const& tag_name) const
{
    return has_in_scope_impl(tag_name, { "html"_fly_string, "table"_fly_string, "template"_fly_string });
}

bool StackOfOpenElements::has_in_list_item_scope(FlyString const& tag_name) const
{
    auto list = s_base_list;
    list.append("ol"_fly_string);
    list.append("ul"_fly_string);
    return has_in_scope_impl(tag_name, list);
}

// https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-select-scope
// The stack of open elements is said to have a particular element in select scope
// when it has that element in the specific scope consisting of all element types except the following:
// - optgroup in the HTML namespace
// - option in the HTML namespace
// NOTE: In this case it's "all element types _except_"
bool StackOfOpenElements::has_in_select_scope(FlyString const& tag_name) const
{
    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-the-specific-scope
    // 1. Initialize node to be the current node (the bottommost node of the stack).
    for (auto& node : m_elements.in_reverse()) {
        // 2. If node is target node, terminate in a match state.
        if (node->local_name() == tag_name)
            return true;
        // 3. Otherwise, if node is one of the element types in list, terminate in a failure state.
        // NOTE: Here "list" refers to all elements except option and optgroup
        if (node->local_name() != HTML::TagNames::option && node->local_name() != HTML::TagNames::optgroup)
            return false;
        // 4. Otherwise, set node to the previous entry in the stack of open elements and return to step 2.
    }
    // NOTE: This will never fail, since the loop will always terminate in the previous step if the top of the stack
    //       — an html element — is reached.
    VERIFY_NOT_REACHED();
}

bool StackOfOpenElements::contains(const DOM::Element& element) const
{
    for (auto& element_on_stack : m_elements) {
        if (&element == element_on_stack.ptr())
            return true;
    }
    return false;
}

bool StackOfOpenElements::contains_template_element() const
{
    for (auto const& element : m_elements) {
        if (element->namespace_uri() != Namespace::HTML)
            continue;
        if (element->local_name() == HTML::TagNames::template_)
            return true;
    }
    return false;
}

void StackOfOpenElements::pop_until_an_element_with_tag_name_has_been_popped(FlyString const& tag_name)
{
    while (m_elements.last()->namespace_uri() != Namespace::HTML || m_elements.last()->local_name() != tag_name)
        (void)pop();
    (void)pop();
}

GC::Ptr<DOM::Element> StackOfOpenElements::topmost_special_node_below(DOM::Element const& formatting_element)
{
    GC::Ptr<DOM::Element> found_element = nullptr;
    for (auto& element : m_elements.in_reverse()) {
        if (element.ptr() == &formatting_element)
            break;
        if (HTMLParser::is_special_tag(element->local_name(), element->namespace_uri()))
            found_element = element.ptr();
    }
    return found_element.ptr();
}

StackOfOpenElements::LastElementResult StackOfOpenElements::last_element_with_tag_name(FlyString const& tag_name)
{
    for (ssize_t i = m_elements.size() - 1; i >= 0; --i) {
        auto& element = m_elements[i];
        if (element->local_name() == tag_name)
            return { element.ptr(), i };
    }
    return { nullptr, -1 };
}

GC::Ptr<DOM::Element> StackOfOpenElements::element_immediately_above(DOM::Element const& target)
{
    bool found_target = false;
    for (auto& element : m_elements.in_reverse()) {
        if (element.ptr() == &target) {
            found_target = true;
        } else if (found_target)
            return element.ptr();
    }
    return nullptr;
}

void StackOfOpenElements::remove(DOM::Element const& element)
{
    m_elements.remove_first_matching([&element](auto& other) {
        return other.ptr() == &element;
    });
}

void StackOfOpenElements::replace(DOM::Element const& to_remove, GC::Ref<DOM::Element> to_add)
{
    for (size_t i = 0; i < m_elements.size(); i++) {
        if (m_elements[i].ptr() == &to_remove) {
            m_elements.remove(i);
            m_elements.insert(i, to_add);
            break;
        }
    }
}

void StackOfOpenElements::insert_immediately_below(GC::Ref<DOM::Element> element_to_add, DOM::Element const& target)
{
    for (size_t i = 0; i < m_elements.size(); i++) {
        if (m_elements[i].ptr() == &target) {
            m_elements.insert(i + 1, element_to_add);
            break;
        }
    }
}

}
