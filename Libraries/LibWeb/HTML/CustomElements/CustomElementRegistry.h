/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/WeakHashSet.h>
#include <LibWeb/Bindings/CustomElementRegistry.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>

namespace Web::HTML {

class CustomElementRegistry;

}

namespace Web::Bindings {

class WrapperWorld;
WEB_API JS::Realm& wrapper_realm_for_custom_element_registry(WrapperWorld const&, JS::Realm&, HTML::CustomElementRegistry&);

}

namespace Web::HTML {

using ElementDefinitionOptions = Bindings::ElementDefinitionOptions;

// https://html.spec.whatwg.org/multipage/custom-elements.html#customelementregistry
class WEB_API CustomElementRegistry : public Bindings::Wrappable {
    WEB_WRAPPABLE(CustomElementRegistry, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CustomElementRegistry);

public:
    [[nodiscard]] static GC::Ref<CustomElementRegistry> create_scoped();
    [[nodiscard]] static GC::Ref<CustomElementRegistry> create_global(DOM::Document&);

    virtual ~CustomElementRegistry() override;

    JS::ThrowCompletionOr<void> define(JS::Realm&, String const& name, WebIDL::CallbackType* constructor, ElementDefinitionOptions const&);
    Variant<GC::Ref<WebIDL::CallbackType>, Empty> get(String const& name) const;
    Optional<String> get_name(GC::Ref<WebIDL::CallbackType> constructor) const;
    GC::Ptr<WebIDL::CallbackType> constructor_for_defined_name(String const& name) const;
    GC::Ptr<WebIDL::Promise> when_defined_promise(String const& name) const;
    void set_when_defined_promise(String const& name, GC::Ref<WebIDL::Promise>);
    void upgrade(GC::Ref<DOM::Node> root) const;
    WebIDL::ExceptionOr<void> initialize_for_bindings(GC::Ref<DOM::Node> root);

    bool is_scoped() const { return m_is_scoped; }
    void append_scoped_document(GC::Ref<DOM::Document>);

    GC::Ptr<CustomElementDefinition> get_definition_with_name_and_local_name(String const& name, String const& local_name) const;
    GC::Ptr<CustomElementDefinition> get_definition_from_new_target(JS::FunctionObject const& new_target) const;

private:
    friend JS::Realm& Bindings::wrapper_realm_for_custom_element_registry(Bindings::WrapperWorld const&, JS::Realm&, CustomElementRegistry&);

    CustomElementRegistry();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<DOM::Document> m_global_document;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#is-scoped
    // Every CustomElementRegistry has an is scoped, a boolean, initially false.
    bool m_is_scoped { false };

    // https://html.spec.whatwg.org/multipage/custom-elements.html#scoped-document-set
    // Every CustomElementRegistry has a scoped document set, a set of Document objects, initially « ».
    // NB: This is a "weak set", see https://github.com/whatwg/html/issues/12092#issuecomment-3769252677
    GC::WeakHashSet<DOM::Document> m_scoped_documents;

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

bool is_a_global_custom_element_registry(GC::Ptr<CustomElementRegistry>);

}
