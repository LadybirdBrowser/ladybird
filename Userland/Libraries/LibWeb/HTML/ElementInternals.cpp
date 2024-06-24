/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ElementInternalsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/ElementInternals.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ElementInternals);

JS::NonnullGCPtr<ElementInternals> ElementInternals::create(JS::Realm& realm, HTMLElement& target_element)
{
    return realm.heap().allocate<ElementInternals>(realm, realm, target_element);
}

ElementInternals::ElementInternals(JS::Realm& realm, HTMLElement& target_element)
    : Bindings::PlatformObject(realm)
    , m_target_element(target_element)
{
}

// https://html.spec.whatwg.org/#dom-elementinternals-shadowroot
JS::GCPtr<DOM::ShadowRoot> ElementInternals::shadow_root() const
{
    // 1. Let target be this's target element.
    auto target = m_target_element;

    // 2. If target is not a shadow host, then return null.
    if (!target->is_shadow_host())
        return nullptr;

    // 3. Let shadow be target's shadow root.
    auto shadow = target->shadow_root();

    // 4. If shadow's available to element internals is false, then return null.
    if (!shadow->available_to_element_internals())
        return nullptr;

    // 5. Return shadow.
    return shadow;
}

void ElementInternals::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ElementInternals);
}

void ElementInternals::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target_element);
}

}
