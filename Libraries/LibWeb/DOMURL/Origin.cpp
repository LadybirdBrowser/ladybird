/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OriginPrototype.h>
#include <LibWeb/DOMURL/Origin.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>

namespace Web::DOMURL {

GC_DEFINE_ALLOCATOR(Origin);

Origin::Origin(JS::Realm& realm, URL::Origin origin)
    : PlatformObject(realm)
    , m_origin(move(origin))
{
}

Origin::~Origin() = default;

void Origin::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Origin);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-constructor
GC::Ref<Origin> Origin::construct_impl(JS::Realm& realm)
{
    // The new Origin() constructor steps are to set this's origin to a unique opaque origin.
    return realm.create<Origin>(realm, URL::Origin::create_opaque());
}

// https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-from
WebIDL::ExceptionOr<GC::Ref<Origin>> Origin::from(JS::VM& vm, JS::Value value)
{
    auto& realm = *vm.current_realm();

    // NB: IDL only ever sees HTML::WindowProxy but we want to use HTML::Window.
    if (auto window_proxy = value.as_if<HTML::WindowProxy>())
        value = window_proxy->window();

    // 1. If value is a platform object:
    if (auto object = value.as_if<Bindings::PlatformObject>()) {
        // 1. Let origin be the result of executing value's extract an origin operation.
        auto origin = object->extract_an_origin();

        // 2. If origin is not null, then return a new Origin object whose origin is origin.
        if (origin.has_value())
            return realm.create<Origin>(realm, origin.release_value());
    }
    // 2. If value is a string:
    else if (value.is_string()) {
        auto string = value.as_string().utf8_string_view();

        // 1. Let parsedURL be the result of basic URL parsing value.
        auto parsed_url = URL::Parser::basic_parse(string);

        // 2. If parsedURL is not failure, then return a new Origin object whose origin is set to parsedURL's origin.
        if (parsed_url.has_value())
            return realm.create<Origin>(realm, parsed_url->origin());
    }

    // 3. Throw a TypeError.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value is not a valid Origin"sv };
}

// https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-opaque
bool Origin::opaque() const
{
    // The opaque getter steps are to return true if this's origin is an opaque origin; otherwise false.
    return m_origin.is_opaque();
}

// https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-issameorigin
bool Origin::is_same_origin(Origin const& other) const
{
    // The isSameOrigin(other) method steps are to return true if this's origin is same origin with other's origin;
    // otherwise false.
    return m_origin.is_same_origin(other.m_origin);
}

// https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-issamesite
bool Origin::is_same_site(Origin const& other) const
{
    // The isSameSite(other) method steps are to return true if this's origin is same site with other's origin;
    // otherwise false.
    return m_origin.is_same_site(other.m_origin);
}

// https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
Optional<URL::Origin> Origin::extract_an_origin() const
{
    // Objects implementing the Origin interface's extract an origin steps are to return this's origin.
    return m_origin;
}

}
