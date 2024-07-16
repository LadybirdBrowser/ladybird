/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>

namespace Web::HTML {

struct ElementDefinitionOptions {
    Optional<String> extends;
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#customelementregistry
class CustomElementRegistry : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CustomElementRegistry, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(CustomElementRegistry);

public:
    virtual ~CustomElementRegistry() override;

    JS::ThrowCompletionOr<void> define(String const& name, WebIDL::CallbackType* constructor, ElementDefinitionOptions options);
    Variant<JS::Handle<WebIDL::CallbackType>, JS::Value> get(String const& name) const;
    Optional<String> get_name(JS::Handle<WebIDL::CallbackType> const& constructor) const;
    WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> when_defined(String const& name);
    void upgrade(JS::NonnullGCPtr<DOM::Node> root) const;

    JS::GCPtr<CustomElementDefinition> get_definition_with_name_and_local_name(String const& name, String const& local_name) const;
    JS::GCPtr<CustomElementDefinition> get_definition_from_new_target(JS::FunctionObject const& new_target) const;

private:
    CustomElementRegistry(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    // Every CustomElementRegistry has a set of custom element definitions, initially empty. In general, algorithms in this specification look up elements in the registry by any of name, local name, or constructor.
    Vector<JS::NonnullGCPtr<CustomElementDefinition>> m_custom_element_definitions;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#element-definition-is-running
    // Every CustomElementRegistry also has an element definition is running flag which is used to prevent reentrant invocations of element definition. It is initially unset.
    bool m_element_definition_is_running { false };

    // https://html.spec.whatwg.org/multipage/custom-elements.html#when-defined-promise-map
    // Every CustomElementRegistry also has a when-defined promise map, mapping valid custom element names to promises. It is used to implement the whenDefined() method.
    OrderedHashMap<String, JS::NonnullGCPtr<JS::Promise>> m_when_defined_promise_map;
};

}
