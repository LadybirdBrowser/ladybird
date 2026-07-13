/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/URLPattern.h>

namespace Web::URLPattern {

using URLPatternInit = Bindings::URLPatternInit;
using URLPatternInput = Variant<Utf16String, Bindings::URLPatternInit>;
using URLPatternResult = Bindings::URLPatternResult;
using URLPatternOptions = Bindings::URLPatternOptions;

// https://urlpattern.spec.whatwg.org/#urlpattern
class URLPattern : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(URLPattern, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(URLPattern);

public:
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create(JS::Realm&, URLPatternInput const&, Optional<Utf16String> const& base_url, URLPatternOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> construct_impl(JS::Realm&, URLPatternInput const&, Utf16String const& base_url, URLPatternOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> construct_impl(JS::Realm&, URLPatternInput const&, URLPatternOptions const& = {});

    WebIDL::ExceptionOr<bool> test(URLPatternInput const&, Optional<Utf16String> const& base_url) const;
    WebIDL::ExceptionOr<Optional<URLPatternResult>> exec(URLPatternInput const&, Optional<Utf16String> const& base_url) const;

    Utf16String protocol() const;
    Utf16String username() const;
    Utf16String password() const;
    Utf16String hostname() const;
    Utf16String port() const;
    Utf16String pathname() const;
    Utf16String search() const;
    Utf16String hash() const;

    bool has_reg_exp_groups() const;

    virtual ~URLPattern() override;

protected:
    virtual void initialize(JS::Realm&) override;

    explicit URLPattern(JS::Realm&, URL::RustIntegration::URLPattern);

private:
    // https://urlpattern.spec.whatwg.org/#ref-for-url-pattern%E2%91%A0
    // Each URLPattern has an associated URL pattern, a URL pattern.
    URL::RustIntegration::URLPattern m_url_pattern;
};

}
