/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Page/AccessKeys.h>

namespace Web {

Optional<AccessKey> AccessKeys::find_by_codepoint(u32 ch)
{
    switch (ch) {
#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) \
    case character:                                                               \
        return AccessKey::name;
        ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY
    }

    return {};
}

Optional<AccessKey> AccessKeys::find_by_keycode(UIEvents::KeyCode code)
{
    switch (code) {
#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) \
    case UIEvents::code:                                                          \
        return AccessKey::name;
        ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY
    default:
        break;
    }
    switch (code) {
#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) \
    case UIEvents::shiftcode:                                                     \
        return AccessKey::name;
        ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY
    default:
        break;
    }

    return {};
}

String AccessKeys::label(AccessKey key)
{
    switch (key) {
#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) \
    case AccessKey::name:                                                         \
        return HTML::AccessKeyNames::name.to_string();
        ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY
    }

    VERIFY_NOT_REACHED();
}

void AccessKeys::assign(DOM::Element& element, AccessKey key)
{
    m_assigned_access_key.ensure(key).append(element);
}

void AccessKeys::unassign(DOM::Element& element)
{
    for (auto& [_, elements] : m_assigned_access_key) {
        elements.remove_all_matching([&](JS::RawGCPtr<DOM::Element> const& other) {
            return other == &element;
        });
    }

    m_assigned_access_key.remove_all_matching([](auto const&, auto const& elements) {
        return elements.is_empty();
    });
}

static inline bool element_or_ancestors_has_hidden_attribute(DOM::Element const& element)
{
    if (element.has_attribute(HTML::AttributeNames::hidden))
        return true;

    if (auto const* parent_element = element.parent_element())
        return element_or_ancestors_has_hidden_attribute(*parent_element);

    return false;
}

// https://html.spec.whatwg.org/multipage/interaction.html#assigned-access-key
bool AccessKeys::trigger_action(AccessKey needle) const
{
    auto must_trigger_action = [](DOM::Element const& element) {
        // When the user presses the key combination corresponding to the assigned access key for an element,
        return
            // if the element defines a command,
            // the command's Hidden State facet is false (visible),
            // the command's Disabled State facet is also false (enabled),
            // FIXME: Commands are not implemented yet.
            !element.is_actually_disabled()
            // the element is in a document that has a non-null browsing context,
            && element.document().browsing_context() != nullptr
            // and neither the element nor any of its ancestors has a hidden attribute specified,
            && !element_or_ancestors_has_hidden_attribute(element);
        // then the user agent must trigger the Action of the command.
    };

    for (auto const& [key, elements] : m_assigned_access_key) {
        if (key == needle) {
            // AD-HOC: Handle the case where multiple elements have the same assigned access key by letting the user to pick one element to trigger, rather than triggering them all at once.
            // This matches the behaviour of other browsers.
            for (size_t i = 0; i < elements.size(); i++) {
                auto const& element = elements[i];
                if (element->is_focused()) {
                    for (size_t j = 1; i < elements.size(); j++) {
                        auto const& next_element = elements[(i + j) % elements.size()];
                        if (must_trigger_action(*next_element)) {
                            next_element->document().set_focused_element(next_element);
                            next_element->document().set_active_element(next_element);
                            return true;
                        }
                    }
                    return false;
                }
            }
            VERIFY(elements.size() > 0);
            auto const& element = elements[0];
            if (must_trigger_action(*element)) {
                element->document().set_focused_element(element);
                element->document().set_active_element(element);
                // TODO: This should "trigger the Action of the command" once they are implemented.
            }
        }
    }

    return false;
}

Optional<AccessKey> AccessKeys::assigned_access_key(DOM::Element const& needle) const
{
    for (auto const& [key, elements] : m_assigned_access_key) {
        for (auto const& element : elements) {
            if (element == &needle) {
                return key;
            }
        }
    }

    return {};
}

void AccessKeys::remove_dead_cells(Badge<JS::Heap>)
{
    for (auto& [_, elements] : m_assigned_access_key) {
        elements.remove_all_matching([](auto const& element) {
            return element->state() != JS::Cell::State::Live;
        });
    }

    m_assigned_access_key.remove_all_matching([](auto const&, auto const& elements) {
        return elements.is_empty();
    });
}

}
