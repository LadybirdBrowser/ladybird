/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibURL/Pattern/Init.h>
#include <LibURL/Pattern/Pattern.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::URLPattern {

using URLPatternInit = URL::Pattern::Init;
using URLPatternInput = URL::Pattern::Input;
using URLPatternOptions = URL::Pattern::Options;
using URLPatternResult = URL::Pattern::Result;

// https://urlpattern.spec.whatwg.org/#urlpattern
class URLPattern : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(URLPattern, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(URLPattern);

public:
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> construct_impl(JS::Realm&, URLPatternInput const&, String const& base_url, URLPatternOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> construct_impl(JS::Realm&, URLPatternInput const&, URLPatternOptions const& = {});

    Optional<URLPatternResult> exec(URLPatternInput const&, Optional<String> const&) const;

    virtual ~URLPattern() override;

protected:
    virtual void initialize(JS::Realm&) override;

    explicit URLPattern(JS::Realm&);
};

}
