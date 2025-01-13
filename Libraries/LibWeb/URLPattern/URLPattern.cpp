/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLPatternPrototype.h>
#include <LibWeb/URLPattern/URLPattern.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::URLPattern {

GC_DEFINE_ALLOCATOR(URLPattern);

URLPattern::URLPattern(JS::Realm& realm)
    : PlatformObject(realm)
{
}

URLPattern::~URLPattern() = default;

void URLPattern::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(URLPattern);
}

WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const&, String const&, URLPatternOptions const&)
{
    return realm.create<URLPattern>(realm);
}

WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const&, URLPatternOptions const&)
{
    return realm.create<URLPattern>(realm);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-exec
Optional<URLPatternResult> URLPattern::exec(URLPatternInput const&, Optional<String> const&) const
{
    dbgln("FIXME: Implement URLPattern::match");
    return {};
}

}
