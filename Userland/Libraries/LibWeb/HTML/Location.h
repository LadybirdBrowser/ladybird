/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginPropertyDescriptorMap.h>
#include <LibWeb/HTML/Navigable.h>

namespace Web::HTML {

class Location final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Location, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Location);

public:
    virtual ~Location() override;

    WebIDL::ExceptionOr<String> href() const;
    WebIDL::ExceptionOr<void> set_href(String const&);

    WebIDL::ExceptionOr<String> origin() const;

    WebIDL::ExceptionOr<String> protocol() const;
    WebIDL::ExceptionOr<void> set_protocol(String const&);

    WebIDL::ExceptionOr<String> host() const;
    WebIDL::ExceptionOr<void> set_host(String const&);

    WebIDL::ExceptionOr<String> hostname() const;
    WebIDL::ExceptionOr<void> set_hostname(String const&);

    WebIDL::ExceptionOr<String> port() const;
    WebIDL::ExceptionOr<void> set_port(String const&);

    WebIDL::ExceptionOr<String> pathname() const;
    WebIDL::ExceptionOr<void> set_pathname(String const&);

    WebIDL::ExceptionOr<String> search() const;
    WebIDL::ExceptionOr<void> set_search(String const&);

    WebIDL::ExceptionOr<String> hash() const;
    WebIDL::ExceptionOr<void> set_hash(String const&);

    WebIDL::ExceptionOr<void> replace(String const& url);
    void reload() const;
    WebIDL::ExceptionOr<void> assign(String const& url);

    WebIDL::ExceptionOr<JS::GCPtr<DOMStringList>> ancestor_origins() const;

    virtual JS::ThrowCompletionOr<JS::Object*> internal_get_prototype_of() const override;
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual JS::ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor const&) override;
    virtual JS::ThrowCompletionOr<JS::Value> internal_get(JS::PropertyKey const&, JS::Value receiver, JS::CacheablePropertyMetadata*, PropertyLookupPhase) const override;
    virtual JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const&, JS::Value value, JS::Value receiver, JS::CacheablePropertyMetadata*) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<JS::MarkedVector<JS::Value>> internal_own_property_keys() const override;

    HTML::CrossOriginPropertyDescriptorMap const& cross_origin_property_descriptor_map() const { return m_cross_origin_property_descriptor_map; }
    HTML::CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map() { return m_cross_origin_property_descriptor_map; }

private:
    explicit Location(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::GCPtr<DOM::Document> relevant_document() const;
    URL::URL url() const;
    WebIDL::ExceptionOr<void> navigate(URL::URL, Bindings::NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior::Auto);

    // [[CrossOriginPropertyDescriptorMap]], https://html.spec.whatwg.org/multipage/browsers.html#crossoriginpropertydescriptormap
    HTML::CrossOriginPropertyDescriptorMap m_cross_origin_property_descriptor_map;

    // [[DefaultProperties]], https://html.spec.whatwg.org/multipage/history.html#defaultproperties
    Vector<JS::Value> m_default_properties;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-location-ancestor-origins-list
    JS::GCPtr<DOMStringList> m_ancestor_origins_list;
};

}
