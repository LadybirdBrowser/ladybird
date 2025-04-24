/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>

namespace Web::HTML {

struct ElementDefinitionOptions {
    Optional<String> extends;
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#customelementregistry
class CustomElementRegistry : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CustomElementRegistry, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CustomElementRegistry);

public:
    [[nodiscard]] static GC::Ref<CustomElementRegistry> construct_impl(JS::Realm&);

    virtual ~CustomElementRegistry() override;

    JS::ThrowCompletionOr<void> define(String const& name, WebIDL::CallbackType* constructor, ElementDefinitionOptions options);
    Variant<GC::Root<WebIDL::CallbackType>, Empty> get(String const& name) const;
    Optional<String> get_name(GC::Root<WebIDL::CallbackType> const& constructor) const;
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> when_defined(String const& name);
    void upgrade(GC::Ref<DOM::Node> root) const;
    void initialize_for_bindings(GC::Ref<DOM::Node> root);

    bool is_scoped() const { return m_is_scoped; }
    void append_scoped_document(GC::Ref<DOM::Document>);

    GC::Ptr<CustomElementDefinition> get_definition_with_name_and_local_name(String const& name, String const& local_name) const;
    GC::Ptr<CustomElementDefinition> get_definition_from_new_target(JS::FunctionObject const& new_target) const;

private:
    CustomElementRegistry(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#is-scoped
    // Every CustomElementRegistry has an is scoped, a boolean, initially false.
    bool m_is_scoped { false };

    // https://html.spec.whatwg.org/multipage/custom-elements.html#scoped-document-set
    // Every CustomElementRegistry has a scoped document set, a set of Document objects, initially « ».
    HashTable<GC::Ref<DOM::Document>> m_scoped_documents;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-definition-set
    // Every CustomElementRegistry has a custom element definition set, a set of custom element definitions,
    // initially « ». Lookup of items in this set uses their name, local name, or constructor.
    Vector<GC::Ref<CustomElementDefinition>> m_custom_element_definitions;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#element-definition-is-running
    // Every CustomElementRegistry also has an element definition is running boolean which is used to prevent reentrant
    // invocations of element definition. It is initially false.
    bool m_element_definition_is_running { false };

    // https://html.spec.whatwg.org/multipage/custom-elements.html#when-defined-promise-map
    // Every CustomElementRegistry also has a when-defined promise map, mapping valid custom element names to promises.
    // It is used to implement the whenDefined() method.
    OrderedHashMap<String, GC::Ref<WebIDL::Promise>> m_when_defined_promise_map;
};

GC::Ptr<CustomElementRegistry> look_up_a_custom_element_registry(DOM::Node const&);
GC::Ptr<CustomElementDefinition> look_up_a_custom_element_definition(GC::Ptr<CustomElementRegistry> registry, Optional<FlyString> const& namespace_, FlyString const& local_name, Optional<String> const& is);

}
