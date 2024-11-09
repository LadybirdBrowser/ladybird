/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::HTML {

struct AlreadyConstructedCustomElementMarker {
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-definition
class CustomElementDefinition : public JS::Cell {
    JS_CELL(CustomElementDefinition, JS::Cell);
    JS_DECLARE_ALLOCATOR(CustomElementDefinition);

    using LifecycleCallbacksStorage = OrderedHashMap<FlyString, JS::GCPtr<WebIDL::CallbackType>>;
    using ConstructionStackStorage = Vector<Variant<JS::Handle<DOM::Element>, AlreadyConstructedCustomElementMarker>>;

    static JS::NonnullGCPtr<CustomElementDefinition> create(JS::Realm& realm, String const& name, String const& local_name, WebIDL::CallbackType& constructor, Vector<String>&& observed_attributes, LifecycleCallbacksStorage&& lifecycle_callbacks, bool form_associated, bool disable_internals, bool disable_shadow)
    {
        return realm.heap().allocate<CustomElementDefinition>(realm, name, local_name, constructor, move(observed_attributes), move(lifecycle_callbacks), form_associated, disable_internals, disable_shadow);
    }

    ~CustomElementDefinition() = default;

    String const& name() const { return m_name; }
    String const& local_name() const { return m_local_name; }

    WebIDL::CallbackType& constructor() { return *m_constructor; }
    WebIDL::CallbackType const& constructor() const { return *m_constructor; }

    Vector<String> const& observed_attributes() const { return m_observed_attributes; }

    LifecycleCallbacksStorage const& lifecycle_callbacks() const { return m_lifecycle_callbacks; }

    ConstructionStackStorage& construction_stack() { return m_construction_stack; }
    ConstructionStackStorage const& construction_stack() const { return m_construction_stack; }

    bool form_associated() const { return m_form_associated; }
    bool disable_internals() const { return m_disable_internals; }
    bool disable_shadow() const { return m_disable_shadow; }

private:
    CustomElementDefinition(String const& name, String const& local_name, WebIDL::CallbackType& constructor, Vector<String>&& observed_attributes, LifecycleCallbacksStorage&& lifecycle_callbacks, bool form_associated, bool disable_internals, bool disable_shadow)
        : m_name(name)
        , m_local_name(local_name)
        , m_constructor(constructor)
        , m_observed_attributes(move(observed_attributes))
        , m_lifecycle_callbacks(move(lifecycle_callbacks))
        , m_form_associated(form_associated)
        , m_disable_internals(disable_internals)
        , m_disable_shadow(disable_shadow)
    {
    }

    virtual void visit_edges(Visitor& visitor) override;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-name
    // A name
    //     A valid custom element name
    String m_name;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-local-name
    // A local name
    //     A local name
    String m_local_name;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-constructor
    // A Web IDL CustomElementConstructor callback function type value wrapping the custom element constructor
    JS::NonnullGCPtr<WebIDL::CallbackType> m_constructor;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-observed-attributes
    // A list of observed attributes
    //     A sequence<DOMString>
    Vector<String> m_observed_attributes;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-lifecycle-callbacks
    // A collection of lifecycle callbacks
    //     A map, whose keys are the strings "connectedCallback", "disconnectedCallback", "adoptedCallback", "attributeChangedCallback",
    //     "formAssociatedCallback", "formDisabledCallback", "formResetCallback", and "formStateRestoreCallback".
    //     The corresponding values are either a Web IDL Function callback function type value, or null.
    //     By default the value of each entry is null.
    LifecycleCallbacksStorage m_lifecycle_callbacks;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-construction-stack
    // A construction stack
    //     A list, initially empty, that is manipulated by the upgrade an element algorithm and the HTML element constructors.
    //     Each entry in the list will be either an element or an already constructed marker.
    ConstructionStackStorage m_construction_stack;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-form-associated
    // A form-associated boolean
    //     If this is true, user agent treats elements associated to this custom element definition as form-associated custom elements.
    bool m_form_associated { false };

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-disable-internals
    // A disable internals boolean
    //     Controls attachInternals().
    bool m_disable_internals { false };

    // https://html.spec.whatwg.org/multipage/custom-elements.html#concept-custom-element-definition-disable-shadow
    // A disable shadow boolean
    //     Controls attachShadow().
    bool m_disable_shadow { false };
};

}
