/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginPropertyDescriptorMap.h>

namespace Web::HTML {

class Location final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Location, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Location);

public:
    virtual ~Location() override;

    WebIDL::ExceptionOr<Utf16String> href() const;
    WebIDL::ExceptionOr<void> set_href(Utf16View);

    WebIDL::ExceptionOr<Utf16String> origin() const;

    WebIDL::ExceptionOr<Utf16String> protocol() const;
    WebIDL::ExceptionOr<void> set_protocol(Utf16View);

    WebIDL::ExceptionOr<Utf16String> host() const;
    WebIDL::ExceptionOr<void> set_host(Utf16View);

    WebIDL::ExceptionOr<Utf16String> hostname() const;
    WebIDL::ExceptionOr<void> set_hostname(Utf16View);

    WebIDL::ExceptionOr<Utf16String> port() const;
    WebIDL::ExceptionOr<void> set_port(Utf16View);

    WebIDL::ExceptionOr<Utf16String> pathname() const;
    WebIDL::ExceptionOr<void> set_pathname(Utf16View);

    WebIDL::ExceptionOr<Utf16String> search() const;
    WebIDL::ExceptionOr<void> set_search(Utf16View);

    WebIDL::ExceptionOr<Utf16String> hash() const;
    WebIDL::ExceptionOr<void> set_hash(Utf16View);

    WebIDL::ExceptionOr<void> replace(Utf16View url);
    void reload() const;
    WebIDL::ExceptionOr<void> assign(Utf16View url);

    virtual JS::ThrowCompletionOr<JS::Object*> internal_get_prototype_of() const override;
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;
    virtual JS::ThrowCompletionOr<bool> internal_is_extensible() const override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual JS::ThrowCompletionOr<JS::Value> internal_get(JS::PropertyKey const&, JS::Value receiver, JS::CacheableGetPropertyMetadata*, PropertyLookupPhase) const override;
    virtual JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const&, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata*, PropertyLookupPhase) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<GC::RootVector<JS::Value>> internal_own_property_keys() const override;

    HTML::CrossOriginPropertyDescriptorMap const& cross_origin_property_descriptor_map() const { return m_cross_origin_property_descriptor_map; }
    HTML::CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map() { return m_cross_origin_property_descriptor_map; }

private:
    explicit Location(JS::Realm&);

    virtual bool is_html_location() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<DOM::Document> relevant_document() const;
    URL::URL url() const;
    WebIDL::ExceptionOr<void> navigate(URL::URL, Bindings::NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior::Auto);

    // [[CrossOriginPropertyDescriptorMap]], https://html.spec.whatwg.org/multipage/browsers.html#crossoriginpropertydescriptormap
    HTML::CrossOriginPropertyDescriptorMap m_cross_origin_property_descriptor_map;

    // [[DefaultProperties]], https://html.spec.whatwg.org/multipage/history.html#defaultproperties
    Vector<JS::PropertyKey> m_default_properties;
};

}

template<>
inline bool JS::Object::fast_is<Web::HTML::Location>() const { return is_html_location(); }
